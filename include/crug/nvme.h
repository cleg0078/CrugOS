#pragma once

#include <crug/types.h>

/* NVMe over PCIe: NVM Express base specification 1.4, polled queues. */
#define NVME_PCI_CLASS      0x01u
#define NVME_PCI_SUBCLASS   0x08u
#define NVME_PCI_PROG_IF    0x02u
#define NVME_MAX_CONTROLLERS 4u
#define NVME_QUEUE_DEPTH    64u

int nvme_init(void);
u32 nvme_device_count(void);
