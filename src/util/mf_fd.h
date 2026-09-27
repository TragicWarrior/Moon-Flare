#ifndef MF_FD_H
#define MF_FD_H

/*
 * Keeping the daemon's descriptors out of the helpers it starts (mf_gatt).
 * A child inherits every descriptor opened without close-on-exec: a serial
 * port, a socket, libcurl's pipes.  A helper that outlives a daemon restart
 * would then hold the XD's /dev/ttyUSB0, say, and the new daemon could not
 * reopen it.
 */

/* The highest descriptor to close by hand, for mf_close_from().  Call it
 * before fork(): it is not async-signal-safe. */
int  mf_fd_limit(void);

/* Close every descriptor from lo up: close_range(2) where the kernel has
 * it, else close() up to limit.  Async-signal-safe, for a child between
 * fork() and exec(). */
void mf_close_from(int lo, int limit);

#endif
