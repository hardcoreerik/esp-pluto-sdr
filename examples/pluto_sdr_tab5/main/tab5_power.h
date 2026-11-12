#pragma once
#include <stdbool.h>

/* Switches the Tab5's USB-A 5 V rail through its second I/O expander (PI4IOE5V6408 at 0x44 on the internal I2C bus, SDA 31 / SCL 32, pin P3).
 * Only that one pin is changed (read-modify-write); every other pin of the expander is left as it was. Returns true when the pin reads back as
 * driven high (enable) or low (disable). Board bring-up for this test only: the driver itself knows nothing about boards. */
bool tab5_usb_power(bool enable);
