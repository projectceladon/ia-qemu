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
#include "qemu/osdep.h"
#include "virtio-video-msdk-enc.h"

size_t virtio_video_enc_cmd_get_params(VirtIODevice *vdev,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    size_t len = 0;

    VIRTVID_DEBUG("    %s: stream 0x%x, queue_type 0x%x", __func__,
            req->hdr.stream_id, req->queue_type);
    if (req != NULL && resp != NULL) {
        resp->hdr.type = req->hdr.type;
        resp->hdr.stream_id = req->hdr.stream_id;
        resp->params.queue_type = req->queue_type;
        len = sizeof(*resp);
    }

    return len;
}

int virtio_video_encode_init(VirtIODevice *vdev)
{
    return 0;
}

void virtio_video_encode_destroy(VirtIODevice *vdev)
{

}
