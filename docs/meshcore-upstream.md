# MeshCore upstream integration

This tree vendors the stable MeshCore companion release so ZIP downloads build
without requiring Git submodules.

- Release: `companion-v1.17.1`
- Commit: `d92964352441e53b93e8667b802e04f6e072b39e`
- Upstream: <https://github.com/meshcore-dev/MeshCore>

The application-specific companion implementation remains in
`src/mesh/companion/`. It derives from and extends MeshCore rather than
modifying the upstream files in `lib/MeshCore`.

## Updating MeshCore

1. Select a stable `companion-vX.Y.Z` tag, not the upstream development branch.
2. Replace `lib/MeshCore` with that tag's contents and retain the exact commit
   above in this document.
3. Update `FIRMWARE_VERSION` in `src/mesh/companion/MyMesh.h` and the displayed
   firmware version in `src/board.h`.
4. Build `t5-epaper` before making unrelated hardware-input changes.
5. Test BLE pairing, radio parameters, GPS settings, identity persistence, and
   a boot with BLE already enabled in NVS.

## BLE-at-boot memory rule

On ESP32-S3, FreeRTOS task stacks consume internal DRAM even when most LVGL
objects and frame buffers live in PSRAM. `setup()` therefore creates the UI task
before it starts MeshCore/BLE. The UI task performs both its initialization and
event loop, so no second late stack allocation can fail after BLE has claimed
internal memory. Do not reverse this ordering without measuring free internal
DRAM and testing a cold boot with BLE enabled.

Bluetooth state changes are serialized by the mesh task. At boot, the UI task
first reserves its internal-DRAM stack but pauses before allocating LVGL and
display resources. MeshCore then constructs and starts the requested transport
before releasing UI initialization. This ordering reserves enough memory for
both tasks while ensuring the BLE controller and complete GATT service are not
starved by later UI allocations.

MeshCore boot initialization runs on Arduino's existing setup task. Do not add
a temporary initialization task: its former 8K-word stack occupied 32 KB of
internal DRAM while ESP-IDF configured BLE advertising, which caused HCI
commands 0x2008 and 0x2009 to fail with `BLE_INIT: Malloc failed`.

MeshCore reuses Arduino's existing loop task rather than allocating another
8 KB permanent stack. Reserving that additional stack before BLE left too
little memory for advertising; allocating it afterward failed and left no
protocol loop. The existing task provides both the memory headroom observed to
complete advertising and a continuously serviced `MyMesh::loop()`.

The UART characteristics require encryption, while the BLE security manager
requests Secure Connections, MITM and bonding with the static PIN. Do not put
`ENC_MITM` directly on the attribute permissions: Android may send its first
MeshCore frame while pairing is completing, causing ESP-IDF to reject it with
`GATT_INSUF_AUTHENTICATION` instead of finishing the exchange.

The e-paper UI task uses a 12 KB stack. Its startup log reports measured stack
headroom; keep a substantial margin when changing UI call depth. The previous
16 KB reservation left only a 7668-byte contiguous block after BLE, which was
insufficient for encrypted notifications and reconnect bookkeeping.
Turning Bluetooth off stops the service, deinitializes Bluedroid and the radio
controller, and releases the controller memory to minimize standby current.
Because ESP-IDF cannot reacquire released controller memory without resetting,
turning Bluetooth on again after a full shutdown performs a controlled reboot;
the persisted ON setting restores Bluetooth automatically after startup.

## Companion custom variables

`CMD_GET_CUSTOM_VARS` is a comma-separated sequence of `name:value` records.
Neither names nor values may contain commas. Keep private option lists in the
device UI (or use a future protocol-defined encoding); an embedded comma makes
the official app parse a fragment with no value and reject the complete reply.

## Message ownership

Messages sent by the companion app are mirrored through `mesh_bridge` into the
PaperUI history as local messages. They are not counted as unread and do not
trigger incoming-message alerts. The v13 companion protocol has no event for a
message sent independently on the device, so PaperUI-originated sends cannot be
truthfully injected into the app as outgoing messages; synthesizing a receive
frame would incorrectly label them as incoming.

## GT911 touch ownership

The GT911 status/point buffer is destructive-read state: after retrieving a
complete point frame, the driver clears register `0x814E`. Only
`board::touch_sample()` may read the controller. LVGL, lock-screen wake handling
and touch diagnostics consume `board::touch_snapshot()` instead. Do not add
direct `isPressed()`/`getTouchPoints()` pairs elsewhere; competing consumers can
clear a short tap before LVGL receives it, particularly when BLE changes task
scheduling.

A quiet GT911 interrupt line means “no new frame,” not “finger released.” The
cached pressed state therefore survives quiet intervals and is cleared only by
a fresh zero-contact frame. This continuity is required for LVGL drag and swipe
recognition.

Panel waveforms run on the dedicated `epd-refresh` task. The worker receives an
immutable PSRAM snapshot rather than the live epdiy front buffer, allowing LVGL
and touch sampling to continue safely during a physical refresh. One active and
one replaceable pending snapshot coalesce rapid UI changes without modifying a
frame that the panel is currently consuming. Keep the worker stack small and
check the startup heap diagnostic whenever BLE memory use changes.
The snapshot buffers must use `heap_caps_aligned_alloc(16, ..., MALLOC_CAP_SPIRAM)`;
epdiy's difference engine requires 16-byte-aligned framebuffer addresses and
will deliberately assert if ordinary PSRAM allocation returns a weaker alignment.
