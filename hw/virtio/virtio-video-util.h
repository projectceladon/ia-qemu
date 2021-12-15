/*
 * Virtio Video Device
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
 * Authors: Colin Xu <colin.xu@intel.com>
 *          Zhuocheng Ding <zhuocheng.ding@intel.com>
 */
#ifndef QEMU_VIRTIO_VIDEO_UTIL_H
#define QEMU_VIRTIO_VIDEO_UTIL_H

#include "hw/virtio/virtio-video.h"

int virtio_video_profile_range(uint32_t format, uint32_t *min, uint32_t *max);
int virtio_video_level_range(uint32_t format, uint32_t *min, uint32_t *max);
void virtio_video_init_format(VirtIOVideoFormat *fmt, uint32_t format);

void virtio_video_report_event(VirtIOVideo *v, uint32_t event,
    uint32_t stream_id);
int virtio_video_cmd_resource_queue_complete(VirtIOVideoWork *work);

#endif /* QEMU_VIRTIO_VIDEO_UTIL_H */
