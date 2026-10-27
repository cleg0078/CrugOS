#include <crug/pci.h>
#include <crug/io.h>

static u32 pci_address(u8 bus, u8 dev, u8 fn, u16 off)
{
    return 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
           ((u32)fn << 8) | ((u32)off & 0xFCu);
}

u32 pci_read32(u8 bus, u8 dev, u8 fn, u16 off)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, dev, fn, off));
    return inl(PCI_CONFIG_DATA);
}

u16 pci_read16(u8 bus, u8 dev, u8 fn, u16 off)
{
    u32 v = pci_read32(bus, dev, fn, off);
    return (u16)(v >> ((off & 2u) * 8u));
}

u8 pci_read8(u8 bus, u8 dev, u8 fn, u16 off)
{
    u32 v = pci_read32(bus, dev, fn, off);
    return (u8)(v >> ((off & 3u) * 8u));
}

void pci_write32(u8 bus, u8 dev, u8 fn, u16 off, u32 value)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, dev, fn, off));
    outl(PCI_CONFIG_DATA, value);
}

void pci_write16(u8 bus, u8 dev, u8 fn, u16 off, u16 value)
{
    u32 old = pci_read32(bus, dev, fn, off);
    u32 shift = (off & 2u) * 8u;
    old &= ~(0xFFFFu << shift);
    old |= (u32)value << shift;
    pci_write32(bus, dev, fn, off, old);
}

int pci_find(u16 vendor, u16 device, u8 *bus, u8 *dev, u8 *fn)
{
    u32 b, d, f;

    for (b = 0; b < 256; b++) {
        for (d = 0; d < 32; d++) {
            u16 v = pci_read16((u8)b, (u8)d, 0, 0x00);
            u8 header;

            if (v == 0xFFFFu)
                continue;
            header = pci_read8((u8)b, (u8)d, 0, 0x0E);
            for (f = 0; f < ((header & 0x80u) ? 8u : 1u); f++) {
                v = pci_read16((u8)b, (u8)d, (u8)f, 0x00);
                if (v == vendor &&
                    pci_read16((u8)b, (u8)d, (u8)f, 0x02) == device) {
                    if (bus) *bus = (u8)b;
                    if (dev) *dev = (u8)d;
                    if (fn) *fn = (u8)f;
                    return 1;
                }
            }
        }
    }
    return 0;
}

void pci_enable(u8 bus, u8 dev, u8 fn, u16 bits)
{
    u16 cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write16(bus, dev, fn, 0x04, (u16)(cmd | bits));
}

u32 pci_bar(u8 bus, u8 dev, u8 fn, int bar)
{
    return pci_read32(bus, dev, fn, (u16)(0x10 + bar * 4));
}
