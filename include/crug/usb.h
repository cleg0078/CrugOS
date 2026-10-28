#ifndef CRUG_USB_H
#define CRUG_USB_H

#include <crug/types.h>
#include <crug/bootinfo.h>

/* USB HID boot keyboard and boot mouse support (phase 5A, phase 5B). */

#define USB_HID_REPORT_BYTES 8u
#define USB_HID_MOUSE_REPORT_BYTES 4u

/* Which boot device the interface describes. */
#define USB_HID_KIND_NONE     0u
#define USB_HID_KIND_KEYBOARD 1u
#define USB_HID_KIND_MOUSE    2u

/* Device kinds that have no HID interrupt endpoint (phase 5C audio). */
#define USB_KIND_OTHER 0u
#define USB_KIND_AUDIO 3u

#define USB_CLASS_AUDIO 0x01u

/* Isochronous endpoints and generic control transfers (phase 5C). */
#define USB_ISOC_PACKETS 32u
#define USB_ISOC_PACKET_MAX 1024u

/* Boot-protocol endpoint discovered inside a configuration descriptor. */
struct usb_hid_endpoint {
    u8 ep_addr;      /* bEndpointAddress, bit 7 set for IN */
    u8 ep_num;       /* endpoint number 1..15 */
    u8 in;           /* 1 when the endpoint is IN */
    u8 kind;         /* USB_HID_KIND_KEYBOARD or USB_HID_KIND_MOUSE */
    u16 max_packet;  /* wMaxPacketSize bits 10:0 */
    u8 interval;     /* bInterval in frames */
};

/* One decoded boot-mouse report: relative motion plus the button state. */
struct usb_hid_pointer {
    i32 dx;
    i32 dy;
    i32 wheel;
    u32 buttons;  /* bit 0 left, bit 1 right, bit 2 middle */
    u32 changes;  /* button bits that changed since the previous report */
};

/* xHCI driver (drivers/usb/xhci.c). */
int usb_init(void);
void usb_cmdline(const boot_info *bi);
void usb_poll(void);
int usb_present(void);
int usb_keyboard_ready(void);
int usb_mouse_ready(void);
int usb_selftest(void);

/* Boot keyboard/mouse descriptor parsing and report decoding
   (drivers/usb/usb_hid.c). */
int usb_hid_parse_config(const u8 *buf, u32 len, struct usb_hid_endpoint *out);
void usb_hid_reset(void);
int usb_hid_boot_decode(const u8 report[USB_HID_REPORT_BYTES]);
int usb_hid_mouse_decode(const u8 *report, u32 len, struct usb_hid_pointer *out);
u32 usb_config_interface_class(const u8 *buf, u32 len, u32 index);

/* Generic endpoint-0 transfers and isochronous endpoints (drivers/usb/xhci.c).
   usb_isoc_submit() takes a packet index below usb_isoc_packets(); the host
   controller owns the packet buffers and exposes them as raw pointers for the
   class driver to fill. Completions are counted in submission order, so the
   driver can refill slot (done + inflight) % packets. */
int usb_control(u32 slot, u8 request_type, u8 request, u16 value, u16 index,
                void *data, u16 len);
int usb_isoc_open(u32 slot, u8 ep_addr, u16 max_packet, u8 interval);
int usb_isoc_submit(u32 slot, u8 ep_addr, u32 index, u32 len);
void *usb_isoc_buf(u32 slot, u8 ep_addr, u32 index);
u64 usb_isoc_buf_pa(u32 slot, u8 ep_addr, u32 index);
u32 usb_isoc_packets(u32 slot, u8 ep_addr);
u64 usb_isoc_done(u32 slot, u8 ep_addr);
u64 usb_isoc_errors(u32 slot, u8 ep_addr);
int usb_isoc_close(u32 slot, u8 ep_addr);

/* Class-driver hooks implemented by drivers/usb/usb_audio.c. */
void usb_audio_attach(u32 slot, u32 port, u32 speed, const u8 *config,
                      u32 config_len);
void usb_audio_detach(u32 slot);
void usb_audio_poll(void);

#endif /* CRUG_USB_H */
