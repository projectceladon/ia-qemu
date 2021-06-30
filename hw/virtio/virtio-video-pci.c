/*
 * VirtIO-Video Backend Driver
 * virtio-video-pci
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
 *
 * Author: Colin Xu <Colin.Xu@intel.com>
 *
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/virtio/virtio-video.h"

static Property virtio_video_pci_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_video_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOVideoPCI *dev = VIRTIO_VIDEO_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);

    qdev_set_parent_bus(vdev, BUS(&vpci_dev->bus), &error_abort);
    object_property_set_bool(OBJECT(vdev), "realized", true, errp);
}

static void virtio_video_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "VirtIO Video Controller";
    device_class_set_props(dc, virtio_video_pci_properties);
    k->realize = virtio_video_pci_realize;
    pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev_k->device_id = PCI_DEVICE_ID_VIRTIO_VIDEO;
    pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
    pcidev_k->class_id = PCI_CLASS_OTHERS;
}

static void virtio_video_pci_instance_init(Object *obj)
{
    VirtIOVideoPCI *dev = VIRTIO_VIDEO_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_VIDEO);
}

static const VirtioPCIDeviceTypeInfo virtio_video_pci_info = {
    .base_name = TYPE_VIRTIO_VIDEO_PCI_BASE,
    .generic_name  = TYPE_VIRTIO_VIDEO_PCI,
    .transitional_name = TYPE_VIRTIO_VIDEO_PCI_TRANS,
    .non_transitional_name = TYPE_VIRTIO_VIDEO_PCI_NON_TRANS,
    .class_init    = virtio_video_pci_class_init,
    .instance_size = sizeof(VirtIOVideoPCI),
    .instance_init = virtio_video_pci_instance_init,
};

static void virtio_video_pci_register(void)
{
    virtio_pci_types_register(&virtio_video_pci_info);
}

type_init(virtio_video_pci_register)
