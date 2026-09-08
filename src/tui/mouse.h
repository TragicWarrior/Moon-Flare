#ifndef MF_MOUSE_H
#define MF_MOUSE_H

#include <ncursesw/curses.h>

/* Dispatch one KEY_MOUSE event (mev from vk_kmio_fetch).
 * Returns 2 if confirm Yes, 1 if consumed, 0 if ignored. */
int mf_mouse_handle(const MEVENT *mev);

#endif
