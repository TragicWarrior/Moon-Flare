/*
 * Opening a Magnum RS-485 tap.  See mag_tty.h.
 *
 * Line settings from pymagnum magnum/magnum.py (Magnum.readPackets:
 * serial_for_url(..., baudrate=19200, bytesize=8, parity=NONE,
 * stopbits=ONE, dsrdtr=False)).
 *   Copyright (c) 2018-2026 Charles Godwin <magnum@godwin.ca>
 *   SPDX-License-Identifier: BSD-3-Clause (third_party/pymagnum/LICENSE)
 * C port for moon-flare (MIT).
 */

#include "mag_tty.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int mag_tty_path_unstable(const char *path)
{
    const char *p;

    if (!path)
        return 0;
    if (strncmp(path, "/dev/ttyUSB", 11) == 0)
        p = path + 11;
    else if (strncmp(path, "/dev/ttyACM", 11) == 0)
        p = path + 11;
    else
        return 0;
    if (!*p)
        return 0;
    for (; *p; p++)
        if (!isdigit((unsigned char)*p))
            return 0;
    return 1;
}

int mag_tty_open(const char *path, char *err, size_t errsz)
{
    struct termios tio;
    struct serial_struct ss;
    int fd;

    fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        if (errno == EBUSY)             /* another reader set TIOCEXCL */
            snprintf(err, errsz, "%.200s is in use by another reader", path);
        else
            snprintf(err, errsz, "%.200s: %.30s", path, strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        snprintf(err, errsz, "%.200s is in use by another reader", path);
        close(fd);
        return -1;
    }
    if (tcgetattr(fd, &tio) != 0)
    {
        snprintf(err, errsz, "%.200s is not a serial port", path);
        close(fd);
        return -1;
    }
    (void)ioctl(fd, TIOCEXCL);
    cfmakeraw(&tio);
    cfsetispeed(&tio, B19200);
    cfsetospeed(&tio, B19200);
    tio.c_cflag &= ~(tcflag_t)(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tio.c_cflag |= (tcflag_t)(CS8 | CLOCAL | CREAD);
    tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 1;                 /* with O_NONBLOCK: EAGAIN, not 0 */
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0)
    {
        snprintf(err, errsz, "%.200s: tcsetattr: %.20s", path, strerror(errno));
        close(fd);
        return -1;
    }
    /* Ask the USB adapter for its shortest latency timer (ftdi_sio: 1 ms
     * instead of 16).  Framing does not depend on it; timestamps in
     * mf_magnum_dump are finer with it.  Not every tty supports it. */
    if (ioctl(fd, TIOCGSERIAL, &ss) == 0)
    {
        ss.flags = (int)((unsigned)ss.flags | ASYNC_LOW_LATENCY);
        (void)ioctl(fd, TIOCSSERIAL, &ss);
    }
    (void)tcflush(fd, TCIFLUSH);        /* pymagnum: reset_input_buffer() */
    return fd;
}

void mag_tty_close(int fd)
{
    if (fd < 0)
        return;
    /* TIOCEXCL belongs to the tty, not the descriptor, and outlives this
     * close while anything else keeps the tty (a pty's master does). */
    (void)ioctl(fd, TIOCNXCL);
    close(fd);                          /* releases the flock too */
}
