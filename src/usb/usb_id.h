#ifndef MF_USB_ID_H
#define MF_USB_ID_H

#include <stddef.h>

/*
 * USB unique-id policy (KD 12 / design §7):
 *
 * Identity, in order:
 *   1. /dev/serial/by-id/ basename + ID_SERIAL_SHORT (text after the last
 *      '_' before "-if" in the by-id name, matching udev).
 *   2. sysfs /sys/class/tty/<tty>/device/../../serial plus
 *      idVendor:idProduct.
 *   3. If neither exists (some CH340s): "usb-VID:PID-ifNN" and treat
 *      auto-port as weak.
 *
 * Resolve is called from the XD plugin on open/error, never every poll.
 * A working path is never replaced "just in case." Empty serial_id +
 * successful open learns serial/by-id without scanning. auto_port:false
 * never scans. Empty serial_id + miss does not walk ttyUSB0..7.
 * Two by-id nodes with the same short serial (CP2102N often "0001")
 * match the full by-id basename; if that is also empty, refuse automode.
 *
 * Persist is mf_endpoint_enqueue only. step() never rename/fsync.
 * Tests inject a fake tree via mf_usb_set_root() and spy the ring.
 */

#define MF_USB_PATH_SIZE   128
#define MF_USB_SERIAL_SIZE 64
#define MF_USB_BYID_SIZE   256
#define MF_USB_UUID_SIZE   40

enum {
    MF_USB_USE      = 0,  /* keep want.path */
    MF_USB_LEARN    = 1,  /* path ok; got has learned serial/by_id */
    MF_USB_RELOCATE = 2,  /* got.path is the new node */
    MF_USB_ERROR    = -1
};

typedef struct {
    char path[MF_USB_PATH_SIZE];
    char serial_id[MF_USB_SERIAL_SIZE];
    char by_id[MF_USB_BYID_SIZE];
    char vid_pid[16];
} mf_usb_id_t;

typedef struct {
    char path[MF_USB_PATH_SIZE];
    char serial_id[MF_USB_SERIAL_SIZE];
    char by_id[MF_USB_BYID_SIZE];
    int  auto_port;
    int  path_ok; /* 1 if open(2)/stat succeeded */
} mf_usb_want_t;

typedef struct {
    char uuid[MF_USB_UUID_SIZE];
    char path[MF_USB_PATH_SIZE];
    char serial_id[MF_USB_SERIAL_SIZE];
    char by_id[MF_USB_BYID_SIZE];
} mf_endpoint_event_t;

/* NULL or "" restores real /dev and /sys. Tests pass a fake tree root. */
void mf_usb_set_root(const char *root);

/* Fill *out for /dev/ttyUSBn (or a basename). 0 ok, -1 no identity. */
int mf_usb_identify(const char *dev_path, mf_usb_id_t *out);

/* Scan /dev/serial/by-id only. 0 unique, -1 none, -2 ambiguous. */
int mf_usb_find_by_serial(const char *serial_id, const char *by_id_hint,
                          mf_usb_id_t *out);

/*
 * Apply the auto-port policy. If uuid is non-empty, LEARN and RELOCATE
 * enqueue an endpoint_changed event. Never writes a config file.
 */
int mf_usb_resolve(const char *uuid, const mf_usb_want_t *want,
                   mf_usb_id_t *got);

int  mf_endpoint_enqueue(const mf_endpoint_event_t *ev);
int  mf_endpoint_drain(mf_endpoint_event_t *out, int max);
void mf_endpoint_reset(void);

#endif
