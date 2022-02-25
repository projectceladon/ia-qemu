/*
 * Virtio video PCI Bindings
 *
 * Copyright (C) 2021, Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Authors: Colin Xu <colin.xu@intel.com>
 *          Zhuocheng Ding <zhuocheng.ding@intel.com>
 */
#include "qemu/osdep.h"

#include "hw/virtio/virtio-video.h"
#include "virtio-pci.h"

typedef struct VirtIOVideoPCI VirtIOVideoPCI;

#define TYPE_VIRTIO_VIDEO_PCI "virtio-video-pci-base"
#define VIRTIO_VIDEO_PCI(obj) \
        OBJECT_CHECK(VirtIOVideoPCI, (obj), TYPE_VIRTIO_VIDEO_PCI)

/*
 * virtio-video-pci: This extends VirtioPCIProxy.
 */
struct VirtIOVideoPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOVideo vdev;
};

static Property virtio_video_pci_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_video_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOVideoPCI *dev = VIRTIO_VIDEO_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);
    printf("%s\n", __func__);

    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_video_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    printf("%s\n", __func__);

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, virtio_video_pci_properties);
    k->realize = virtio_video_pci_realize;
    pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev_k->device_id = PCI_DEVICE_ID_VIRTIO_VIDEO;
    pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
    pcidev_k->class_id = PCI_CLASS_MULTIMEDIA_OTHER;
}

static void virtio_video_pci_instance_init(Object *obj)
{
    VirtIOVideoPCI *dev = VIRTIO_VIDEO_PCI(obj);

    printf("%s\n", __func__);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_VIDEO);
}

static const VirtioPCIDeviceTypeInfo virtio_video_pci_info = {
    .base_name = TYPE_VIRTIO_VIDEO_PCI,
    .generic_name  = "virtio-video-pci",
    .transitional_name = "virtio-video-pci-transitional",
    .non_transitional_name = "virtio-video-pci-non-transitional",
    .class_init    = virtio_video_pci_class_init,
    .instance_size = sizeof(VirtIOVideoPCI),
    .instance_init = virtio_video_pci_instance_init,
};

static void virtio_video_pci_register(void)
{
    printf("%s\n", __func__);
    virtio_pci_types_register(&virtio_video_pci_info);
}

type_init(virtio_video_pci_register)
