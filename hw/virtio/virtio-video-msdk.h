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
#ifndef QEMU_VIRTIO_VIDEO_MSDK_H
#define QEMU_VIRTIO_VIDEO_MSDK_H

#include "mfx/mfxsession.h"
#include "mfx/mfxstructures.h"
#include "mfx/mfxvp8.h"
#include "mfx/mfxvideo.h"
#include "mfx/mfxplugin.h"
#include "hw/virtio/virtio-video.h"

#define VIRTIO_VIDEO_MSDK_DIMENSION_MAX        8192
#define VIRTIO_VIDEO_MSDK_DIMENSION_MIN        16
#define VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE 16
#define VIRTIO_VIDEO_MSDK_DIM_STEP_OTHER       32

#define MSDK_ALIGN16(value) (((value + 15) >> 4) << 4)
#define MSDK_ALIGN32(X) (((mfxU32)((X)+31)) & (~ (mfxU32)31))

typedef struct VirtIOVideoMSDKFrameRange {
    virtio_video_format format;
    struct {
        mfxU16 width;
        mfxU16 height;
    } min;
    struct {
        mfxU16 width;
        mfxU16 height;
    } max;
} VirtIOVideoMSDKFrameRange;

typedef struct VirtIOVideoMediaSdk {
} VirtIOVideoMediaSdk;

void virtio_video_msdk_fill_video_params(virtio_video_format format, mfxVideoParam *param);
void virtio_video_msdk_fill_format_desc(virtio_video_format format, virtio_video_format_desc *format_desc);
bool virtio_video_msdk_find_format(VirtIOVideoCaps *caps, virtio_video_format format, virtio_video_format_desc **format_desc);
bool virtio_video_msdk_find_format_desc(VirtIOVideoCaps *caps, virtio_video_format_desc *format_desc);
int virtio_video_msdk_get_plugin(virtio_video_format format, bool encode, mfxPluginUID *plugin);
void virtio_video_msdk_load_plugin(VirtIODevice *vdev, mfxSession mfx_session, virtio_video_format format, bool encode, bool unload);
int virito_video_format_to_mfx4cc(virtio_video_format fmt);
virtio_video_format virito_video_format_from_mfx4cc(int mfx4cc);
void virtio_video_profile_range(virtio_video_format fmt, int *min, int *max);
void virtio_video_level_range(virtio_video_format fmt, int *min, int *max);
int virtio_video_profile_to_mfx(virtio_video_format fmt, virtio_video_profile profile);
int virtio_video_level_to_mfx(virtio_video_format fmt, virtio_video_level level);

#endif /* QEMU_VIRTIO_VIDEO_MSDK_H */
