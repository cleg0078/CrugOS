#ifndef CRUG_USB_H
#define CRUG_USB_H

#include <crug/types.h>
#include <crug/bootinfo.h>

/* USB HID boot keyboard support (phase 5A). */

#define USB_HID_REPORT_BYTES 8u

/* Boot-protocol keyboard endpoint discovered inside a configuration descriptor. */
struct usb_hid_endpoint {
    u8 ep_addr;      /* bEndpointAddress, bit 7 set for IN */
    u8 ep_num;       /* endpoint number 1..15 */
    u8 in;           /* 1 when the endpoint is IN */
    u16 max_packet;  /* wMaxPacketSize bits 10:0 */
    u8 interval;     /* bInterval in frames */
};

/* xHCI driver (drivers/usb/xhci.c). */
int usb_init(void);
void usb_cmdline(const boot_info *bi);
void usb_poll(void);
int usb_present(void);
int usb_keyboard_ready(void);
int usb_selftest(void);

/* Boot keyboard descriptor parsing and report decoding (drivers/usb/usb_hid.c). */
int usb_hid_parse_config(const u8 *buf, u32 len, struct usb_hid_endpoint *out);
void usb_hid_reset(void);
int usb_hid_boot_decode(const u8 report[USB_HID_REPORT_BYTES]);

#endif /* CRUG_USB_H */
