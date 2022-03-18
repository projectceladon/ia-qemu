/*
 * Virtio Video Device
 *
 * Copyright (C) 2022, Intel Corporation. All rights reserved.
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
 * Authors: Dong Yang <dong.yang@intel.com>
 */
#ifndef QEMU_VIRTIO_VIDEO_VA_ALLOCATOR_H
#define QEMU_VIRTIO_VIDEO_VA_ALLOCATOR_H

#include "mfx/mfxvideo.h"

// VAAPI Allocator internal Mem ID
typedef struct {
    VASurfaceID *m_surface;
    VAImage m_image;
    mfxFrameSurface1 *m_frame;
    // variables for VAAPI Allocator internal color conversion
    unsigned int m_fourcc;
    mfxU8 *m_sys_buffer;
    mfxU8 *m_va_buffer;
    // buffer info to support surface export
    VABufferInfo m_buffer_info;
    // pointer to private export data
    void *m_custom;
} vaapiMemId;

mfxStatus virtio_video_frame_alloc(mfxHDL pthis, mfxFrameAllocRequest *request,
                                   mfxFrameAllocResponse *response);
mfxStatus virtio_video_frame_free(mfxHDL pthis,
                                  mfxFrameAllocResponse *response);
mfxStatus virtio_video_frame_lock(mfxHDL pthis, mfxMemId mid,
                                  mfxFrameData *frame_data);
mfxStatus virtio_video_frame_unlock(mfxHDL pthis, mfxMemId mid,
                                    mfxFrameData *frame_data);
mfxStatus virtio_video_frame_get_handle(mfxHDL pthis, mfxMemId mid,
                                        mfxHDL *handle);

#endif