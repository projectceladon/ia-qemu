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
#ifndef QEMU_VIRTIO_VIDEO_MSDK_UTIL_H
#define QEMU_VIRTIO_VIDEO_MSDK_UTIL_H

#include "mfx/mfxsession.h"
#include "mfx/mfxstructures.h"
#include "hw/virtio/virtio-video.h"

#define MSDK_ALIGN16(value) (((value + 15) >> 4) << 4)
#define MSDK_ALIGN32(X) (((mfxU32)((X)+31)) & (~ (mfxU32)31))

uint32_t virtio_video_format_to_msdk(uint32_t format);
uint32_t virtio_video_profile_to_msdk(uint32_t profile);
uint32_t virtio_video_level_to_msdk(uint32_t level);
int virtio_video_profile_range(uint32_t format, uint32_t *min, uint32_t *max);
int virtio_video_level_range(uint32_t format, uint32_t *min, uint32_t *max);
void virtio_video_msdk_init_video_params(mfxVideoParam *param, uint32_t format);
void virtio_video_msdk_init_format(VirtIOVideoFormat *fmt, uint32_t format);
void virtio_video_msdk_load_plugin(mfxSession session, uint32_t format, bool encode);
void virtio_video_msdk_unload_plugin(mfxSession session, uint32_t format, bool encode);

#endif /* QEMU_VIRTIO_VIDEO_MSDK_UTIL_H */
