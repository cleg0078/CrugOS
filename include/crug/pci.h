#pragma once

#include <crug/types.h>

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA    0xCFCu

#define PCI_COMMAND_IO      0x0001u
#define PCI_COMMAND_MEMORY  0x0002u
#define PCI_COMMAND_MASTER  0x0004u

#define PCI_BAR_IO          0x1u

u32 pci_read32(u8 bus, u8 dev, u8 fn, u16 off);
u16 pci_read16(u8 bus, u8 dev, u8 fn, u16 off);
u8 pci_read8(u8 bus, u8 dev, u8 fn, u16 off);
void pci_write32(u8 bus, u8 dev, u8 fn, u16 off, u32 value);
void pci_write16(u8 bus, u8 dev, u8 fn, u16 off, u16 value);
int pci_find(u16 vendor, u16 device, u8 *bus, u8 *dev, u8 *fn);
int pci_find_nth(u16 vendor, u16 device, int index, u8 *bus, u8 *dev, u8 *fn);
void pci_enable(u8 bus, u8 dev, u8 fn, u16 bits);
u32 pci_bar(u8 bus, u8 dev, u8 fn, int bar);
