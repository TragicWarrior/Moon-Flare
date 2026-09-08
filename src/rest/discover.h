#ifndef MF_DISCOVER_H
#define MF_DISCOVER_H

#include <stddef.h>
#include <sys/select.h>

/* One in-flight discover job. POST starts; GET polls; step from the
 * daemon tick, never from an HTTP protothread. */

int  mf_discover_start(const char *body, size_t body_len, char *err, size_t errsz);
int  mf_discover_result(char *json, size_t cap);
int  mf_discover_is_running(void);
void mf_discover_prepare_fds(fd_set *r, fd_set *w, int *maxfd);
void mf_discover_step(void);
void mf_discover_close(void);

#endif
