#ifndef POWER_H
#define POWER_H

/* Reset the whole SoC through the BCM2711 power-management watchdog. The firmware
 * then boots again from the SD card, so the bootloader is back and a new image can
 * be uploaded without unplugging the power supply. Does not return. */
void system_reset(void) __attribute__((noreturn));

#endif
