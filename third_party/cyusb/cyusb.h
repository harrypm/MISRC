#ifndef MISRC_CYUSB_COMPAT_H
#define MISRC_CYUSB_COMPAT_H

/*
 * cyusb compatibility shim (vendored)
 *
 * Drop-in replacement for the Cypress cyusb 1.0.5 library API used by MISRC's
 * FX3 capture backend (misrc_gui/input/gui_fx3.c). The real Cypress cyusb
 * library is not packaged by Linux distros; this shim implements the small API
 * surface MISRC uses (6 functions + 1 type) as thin wrappers over libusb-1.0.
 *
 * cyusb_handle is an alias for libusb's opaque `struct libusb_device_handle`
 * so that existing call sites in gui_fx3.c which pass a cyusb_handle* directly
 * to libusb_get_device(), libusb_control_transfer(), libusb_clear_halt(), etc.
 * compile unchanged.
 *
 * Lifecycle (matches gui_fx3.c usage):
 *   cyusb_open()       - init libusb context, enumerate + open all devices,
 *                        return device count (re-entrant: closes any prior
 *                        session first). Returns a negative libusb error code
 *                        on context/list init failure.
 *   cyusb_gethandle(i) - return the pre-opened handle at index i, or NULL if
 *                        i is out of range or that device could not be opened.
 *   cyusb_close()      - close all handles opened by cyusb_open(), free the
 *                        device list, exit the libusb context. Safe to call
 *                        when no session is open.
 *   cyusb_claim_interface / cyusb_release_interface / cyusb_bulk_transfer
 *                      - direct libusb-1.0 passthroughs.
 */

/* Pull in libusb the same portable way misrc_tools/common/libusb_compat.h does
 * (most Linux/macOS expose <libusb-1.0/libusb.h>, some Windows toolchains
 * expose <libusb.h>). */
#if defined(__has_include)
#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#elif __has_include(<libusb.h>)
#include <libusb.h>
#else
#error "libusb header not found (tried <libusb-1.0/libusb.h> and <libusb.h>)"
#endif
#elif defined(_WIN32)
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Alias so cyusb_handle* is interchangeable with libusb_device_handle*.
 * libusb forward-declares `struct libusb_device_handle` as an opaque type, so
 * this typedef makes cyusb_handle* the same pointer type as
 * libusb_device_handle*. */
typedef struct libusb_device_handle cyusb_handle;

/* Enumerate and open all USB devices on the system. Returns the device count,
 * or a negative libusb error code on context/list init failure. Re-entrant: a
 * prior session is torn down first so repeated enumerate/open sequences do not
 * leak handles. */
int cyusb_open(void);

/* Release all handles and the libusb context opened by cyusb_open(). */
void cyusb_close(void);

/* Return the handle opened for device index i during cyusb_open(), or NULL if
 * i is out of range or that device could not be opened (e.g. no permissions). */
cyusb_handle *cyusb_gethandle(int index);

/* Interface claim/release - direct libusb passthroughs. */
int cyusb_claim_interface(cyusb_handle *h, int interface_number);
int cyusb_release_interface(cyusb_handle *h, int interface_number);

/* Bulk transfer - direct libusb passthrough with the same signature as
 * libusb_bulk_transfer(). */
int cyusb_bulk_transfer(cyusb_handle *h, unsigned char endpoint,
                        unsigned char *data, int length,
                        int *actual_length, unsigned int timeout);

#ifdef __cplusplus
}
#endif

#endif /* MISRC_CYUSB_COMPAT_H */
