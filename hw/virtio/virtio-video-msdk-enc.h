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
 * Authors: Colin Xu <colin.xu@intel.com>
 *          Zhuocheng Ding <zhuocheng.ding@intel.com>
 */
#ifndef QEMU_VIRTIO_VIDEO_MSDK_ENC_H
#define QEMU_VIRTIO_VIDEO_MSDK_ENC_H

#include "hw/virtio/virtio-video.h"

size_t virtio_video_msdk_enc_stream_create(VirtIOVideo *v,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp);
size_t virtio_video_msdk_enc_stream_destroy(VirtIOVideo* v,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp, 
    VirtQueueElement *elem);
size_t virtio_video_msdk_enc_stream_drain(VirtIOVideo *v,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem);
size_t virtio_video_msdk_enc_resource_queue(VirtIOVideo *v,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp,
    VirtQueueElement *elem);
size_t virtio_video_msdk_enc_resource_destroy_all(VirtIOVideo *v,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp, 
    VirtQueueElement *elem);
size_t virtio_video_msdk_enc_queue_clear(VirtIOVideo *v,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp, 
    VirtQueueElement *elem);
size_t virtio_video_msdk_enc_get_params(VirtIOVideo *v,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp);
size_t virtio_video_msdk_enc_set_params(VirtIOVideo *v,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp);
size_t virtio_video_msdk_enc_query_control(VirtIOVideo *v,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp);
size_t virtio_video_msdk_enc_get_control(VirtIOVideo *v,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp);
size_t virtio_video_msdk_enc_set_control(VirtIOVideo *v,
    virtio_video_set_control *req, virtio_video_set_control_resp *resp);


int virtio_video_init_msdk_enc(VirtIOVideo *v);
void virtio_video_uninit_msdk_enc(VirtIOVideo *v);

#endif /* QEMU_VIRTIO_VIDEO_MSDK_ENC_H */
