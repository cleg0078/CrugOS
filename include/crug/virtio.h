#pragma once

#include <crug/types.h>

#define VIRTIO_PCI_VENDOR 0x1AF4u
#define VIRTIO_PCI_DEVICE_BLOCK 0x1001u

#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER      2u
#define VIRTIO_STATUS_DRIVER_OK   4u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_FAILED      128u

#define VIRTIO_BLK_T_IN  0u
#define VIRTIO_BLK_T_OUT 1u

int virtio_blk_init(void);
