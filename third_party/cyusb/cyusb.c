/*
 * cyusb compatibility shim - implementation over libusb-1.0
 *
 * Implements the Cypress cyusb 1.0.5 API surface used by MISRC's FX3 backend
 * as thin wrappers over libusb-1.0. See cyusb.h for the lifecycle rationale.
 */

#include "cyusb.h"

#include <stddef.h>

/* Internal session state. cyusb_open() creates a fresh session and
 * cyusb_close() tears it down. Re-entrant: cyusb_open() while a session is
 * still active first closes the prior session so repeated enumerate/open
 * sequences (e.g. device-dropdown enumeration followed by Connect) do not
 * leak handles or contexts. */

#define CYUSB_MAX_DEVICES 256

static libusb_context *s_ctx = NULL;
static libusb_device **s_devlist = NULL;
static ssize_t s_devcount = 0;
static cyusb_handle *s_handles[CYUSB_MAX_DEVICES];

int cyusb_open(void)
{
    /* Re-entrant teardown of any prior session. */
    if (s_ctx != NULL || s_devlist != NULL) {
        cyusb_close();
    }

    for (int i = 0; i < CYUSB_MAX_DEVICES; i++) {
        s_handles[i] = NULL;
    }

#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x0100010A)
    int r = libusb_init_context(&s_ctx, NULL, 0);
#else
    int r = libusb_init(&s_ctx);
#endif
    if (r != 0) {
        s_ctx = NULL;
        return r;
    }

    s_devcount = libusb_get_device_list(s_ctx, &s_devlist);
    if (s_devcount < 0) {
        int err = (int)s_devcount;
        libusb_exit(s_ctx);
        s_ctx = NULL;
        s_devlist = NULL;
        return err;
    }

    /* Open each device; leave NULL for ones that fail to open (e.g. no udev
     * permissions). cyusb_gethandle() returns NULL for those and gui_fx3.c
     * skips NULL handles. Matches the real cyusb behaviour of pre-opening
     * every device during cyusb_open(). */
    ssize_t n = s_devcount;
    if (n > CYUSB_MAX_DEVICES) {
        n = CYUSB_MAX_DEVICES;
    }
    for (ssize_t i = 0; i < n; i++) {
        libusb_device_handle *h = NULL;
        if (libusb_open(s_devlist[i], &h) == 0) {
            s_handles[i] = h;
        } else {
            s_handles[i] = NULL;
        }
    }

    return (int)s_devcount;
}

void cyusb_close(void)
{
    for (int i = 0; i < CYUSB_MAX_DEVICES; i++) {
        if (s_handles[i]) {
            libusb_close(s_handles[i]);
            s_handles[i] = NULL;
        }
    }

    if (s_devlist) {
        libusb_free_device_list(s_devlist, 1);
        s_devlist = NULL;
    }
    s_devcount = 0;

    if (s_ctx) {
        libusb_exit(s_ctx);
        s_ctx = NULL;
    }
}

cyusb_handle *cyusb_gethandle(int index)
{
    if (index < 0 || index >= CYUSB_MAX_DEVICES) {
        return NULL;
    }
    if (index >= (int)s_devcount) {
        return NULL;
    }
    return s_handles[index];
}

int cyusb_claim_interface(cyusb_handle *h, int interface_number)
{
    return libusb_claim_interface(h, interface_number);
}

int cyusb_release_interface(cyusb_handle *h, int interface_number)
{
    return libusb_release_interface(h, interface_number);
}

int cyusb_bulk_transfer(cyusb_handle *h, unsigned char endpoint,
                        unsigned char *data, int length,
                        int *actual_length, unsigned int timeout)
{
    return libusb_bulk_transfer(h, endpoint, data, length, actual_length, timeout);
}
