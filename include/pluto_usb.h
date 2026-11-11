#pragma once
/* USB host transport for a PlutoSDR (libiio "usb" backend): finds the device (VID 0456, PID b673), claims its libiio
 * interface (class 02, six bulk endpoints = three pipes), opens pipe 0 and exposes it as a pluto_io_t so the same iiod
 * client used over TCP runs over USB. ESP-IDF only (needs the `usb` component); the pure parts of this library do not.
 *
 * One Pluto at a time. The host library may already be installed by someone else (for example the RTL-SDR driver in
 * a firmware that also supports dongles): pass install_host=false and this registers only an additional client.
 * Nothing here knows about any application. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pluto_iiod.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLUTO_USB_VID 0x0456
#define PLUTO_USB_PID 0xb673

/* Registers a USB host client (and installs the host library when install_host is true). Call once. 0 or a negative
 * errno-style value. */
int pluto_usb_init(bool install_host);

/* True while a Pluto is attached and has been seen by pluto_usb_poll(). */
bool pluto_usb_present(void);

/* Looks for an attached Pluto among the devices on the bus (call about once a second while waiting). Returns true
 * when one is attached and held open by this transport. */
bool pluto_usb_poll(void);

/* Claims the libiio interface of the attached Pluto and opens pipe 0. On success fills `io`, which stays valid
 * until pluto_usb_close(). */
int pluto_usb_open(pluto_io_t *io);

/* Releases the interface and the device. Safe to call when nothing is open. */
void pluto_usb_close(void);

/* Strings and speed of the attached device (any may stay empty). */
void pluto_usb_describe(char *manufacturer, size_t mcap, char *product, size_t pcap, char *serial, size_t scap, bool *high_speed);

/* True once the device was unplugged or a transfer showed it gone (an open session must then be closed). */
bool pluto_usb_gone(void);

#ifdef __cplusplus
}
#endif
