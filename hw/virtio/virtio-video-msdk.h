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
#include "hw/virtio/virtio-video.h"

#define VIRTIO_VIDEO_MSDK_VERSION_MAJOR         1
#define VIRTIO_VIDEO_MSDK_VERSION_MINOR         0

#define VIRTIO_VIDEO_MSDK_DIMENSION_MAX         8192
#define VIRTIO_VIDEO_MSDK_DIMENSION_MIN         16
#define VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE  16
#define VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS       32

#define VIRTIO_VIDEO_MSDK_TIME_TO_WAIT          500

typedef struct MsdkSurface {
    mfxFrameSurface1 surface;
    bool used;
    QLIST_ENTRY(MsdkSurface) next;
} MsdkSurface;

/**
 * Represents a frame being decoded or encoded.
 *
 * @surface: points to the working frame surface in @surface_pool
 * @vpp_surface: points to the working frame surface in @vpp_surface_pool
 */
typedef struct MsdkFrame {
    MsdkSurface *surface;
    MsdkSurface *vpp_surface;
    mfxSyncPoint sync;
    mfxSyncPoint vpp_sync;
} MsdkFrame;

/**
 * @bitstream: the input bitstream for decoder
 * @surface_num: determines the initial size of @surface_pool
 * @vpp_surface_num: determines the initial size of @vpp_surface_pool
 */
typedef struct MsdkSession {
    QemuThread input_thread;
    QemuThread output_thread;
    QemuEvent input_notifier;
    QemuEvent output_notifier;
    mfxSession session;
    mfxBitstream bitstream;
    bool input_accepted;
    int surface_num;
    int vpp_surface_num;
    QLIST_HEAD(, MsdkSurface) surface_pool;
    QLIST_HEAD(, MsdkSurface) vpp_surface_pool;
} MsdkSession;

typedef struct MsdkHandle {
    int drm_fd;
    void *va_handle;
} MsdkHandle;

size_t virtio_video_msdk_cmd_stream_create(VirtIOVideo *v,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp);
size_t virtio_video_msdk_cmd_stream_destroy(VirtIOVideo *v,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem);
size_t virtio_video_msdk_cmd_stream_drain(VirtIOVideo *v,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem);
size_t virtio_video_msdk_cmd_resource_queue(VirtIOVideo *v,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp,
    VirtQueueElement *elem);
size_t virtio_video_msdk_cmd_resource_destroy_all(VirtIOVideo *v,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem);
size_t virtio_video_msdk_cmd_queue_clear(VirtIOVideo *v,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem);
size_t virtio_video_msdk_cmd_get_params(VirtIOVideo *v,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp);
size_t virtio_video_msdk_cmd_set_params(VirtIOVideo *v,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp);
size_t virtio_video_msdk_cmd_query_control(VirtIOVideo *v,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp);
size_t virtio_video_msdk_cmd_get_control(VirtIOVideo *v,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp);
size_t virtio_video_msdk_cmd_set_control(VirtIOVideo *v,
    virtio_video_set_control *req, virtio_video_set_control_resp *resp);

int virtio_video_init_msdk(VirtIOVideo *v);
void virtio_video_uninit_msdk(VirtIOVideo *v);

#endif /* QEMU_VIRTIO_VIDEO_MSDK_H */
