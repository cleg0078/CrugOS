#ifndef CRUG_AHCI_H
#define CRUG_AHCI_H

#include <crug/types.h>

/* AHCI/SATA block driver: polled command list, LBA48 DMA transfers. */
#define AHCI_PCI_CLASS     0x01u
#define AHCI_PCI_SUBCLASS  0x06u
#define AHCI_PCI_PROG_IF   0x01u
#define AHCI_MAX_CONTROLLERS 4u
#define AHCI_MAX_DEVICES   8u

int ahci_init(void);
u32 ahci_device_count(void);

#endif

