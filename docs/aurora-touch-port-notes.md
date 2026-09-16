# Aurora touch port notes

Touch has deliberately not been changed in the MeshCore 1.17.1 checkpoint.
Keeping this boundary makes radio/BLE regressions distinguishable from input
regressions.

The reference implementation is CrossPoint Aurora's FreeInk SDK
`InputManager`, using the `LILYGO_T5_PRO_GT911` board profile. A later port into
PaperUI's LVGL input driver should preserve these behaviours:

- initialise I2C on SDA 39/SCL 40 at 400 kHz with a short transaction timeout;
- perform the GT911 reset/INT address-selection sequence;
- probe both controller addresses, `0x5D` and `0x14`;
- read ready/status register `0x814E`, then coherent 8-byte contact records at
  `0x8150`, and clear the status after consuming a frame;
- keep the last complete contact state across transient I2C read failures;
- transform T5 coordinates by swapping X/Y and flipping Y before passing them
  into the display/UI coordinate space;
- report distinct press, hold, release, tap, and gesture states;
- serialize access to the I2C bus shared with the PMIC, RTC, and fuel gauge.

PaperUI currently uses `TouchDrvGT911` and a 50 ms LVGL polling callback. The
port should replace that single touch path rather than add a second competing
driver, while retaining LVGL as the consumer of pointer events.
