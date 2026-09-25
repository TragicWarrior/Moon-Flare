#ifndef MF_MAG_TTY_H
#define MF_MAG_TTY_H

/*
 * Opening a Magnum RS-485 tap: read-only, exclusive, pymagnum's line
 * settings (19200 baud, 8 data bits, no parity, 1 stop bit, no flow
 * control: magnum/magnum.py Magnum.readPackets).
 *   Copyright (c) 2018-2026 Charles Godwin <magnum@godwin.ca>
 *   SPDX-License-Identifier: BSD-3-Clause (third_party/pymagnum/LICENSE)
 * C port for moon-flare (MIT).
 *
 * The descriptor is O_RDONLY, so nothing can ever be written to the bus.
 * It is non-blocking with VMIN=1/VTIME=0: read() gives EAGAIN when there is
 * nothing to read and 0 only when the adapter has gone (a hangup).  It holds
 * an exclusive flock and TIOCEXCL, so no second reader (another module,
 * mf_magnum_dump, a restarted prototype daemon) can open the port after it.
 */

#include <stddef.h>

#define MAG_BAUD 19200

/* The descriptor, or -1 with a reason in err. */
int  mag_tty_open(const char *path, char *err, size_t errsz);
/* Clears TIOCEXCL, then closes: the next reader can open the port. */
void mag_tty_close(int fd);

/* /dev/ttyUSBn, /dev/ttyACMn: kernel names that move between boots. */
int  mag_tty_path_unstable(const char *path);

#endif
