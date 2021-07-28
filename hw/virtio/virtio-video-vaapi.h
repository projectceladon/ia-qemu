/*
 * VirtIO-Video Backend Driver
 * VirtIO-Video Backend LIBVA
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
#ifndef QEMU_VIRTIO_VIDEO_VAAPI_H
#define QEMU_VIRTIO_VIDEO_VAAPI_H

#include "hw/virtio/virtio-video.h"

int virtio_video_create_va_env_drm(VirtIODevice *vdev);
void virtio_video_destroy_va_env_drm(VirtIODevice *vdev);
void virtio_video_vaapi_query_caps(virtio_video_format fmt);

#endif /* QEMU_VIRTIO_VIDEO_VAAPI_H */
