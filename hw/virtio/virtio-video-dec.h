/*
 * VirtIO-Video Backend Driver
 * VirtIO-Video Backend Decoder Defines
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
#ifndef QEMU_VIRTIO_VIDEO_DEC_H
#define QEMU_VIRTIO_VIDEO_DEC_H

#include "hw/virtio/virtio-video.h"
#include "virtio-video-msdk.h"

size_t virtio_video_dec_cmd_query_capability(VirtIODevice *vdev,
    virtio_video_query_capability *req, virtio_video_query_capability_resp **resp);
size_t virtio_video_dec_cmd_stream_create(VirtIODevice *vdev,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp);
size_t virtio_video_dec_cmd_stream_destroy(VirtIODevice *vdev,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp);
size_t virtio_video_dec_cmd_get_params(VirtIODevice *vdev,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp);
size_t virtio_video_dec_cmd_get_control(VirtIODevice *vdev,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp);
size_t virtio_video_dec_event(VirtIODevice *vdev, virtio_video_event *ev);

// Media-SDK wrapper
int virtio_video_decode_init(VirtIODevice *vdev);
void virtio_video_decode_destroy(VirtIODevice *vdev);

#endif /* QEMU_VIRTIO_VIDEO_DEC_H */
