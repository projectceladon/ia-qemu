/*
 * VirtIO-Video Backend Driver
 * VirtIO-Video Backend Decoder
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
#include "virtio-video-dec.h"

/* Static caps, should fetch from underlay media framework */
static struct {
    struct virtio_video_query_capability_resp resp;
    /* Nested caps: desc->frame->range */
    struct virtio_video_format_desc desc0;
        struct virtio_video_format_frame frame0_0;
            struct virtio_video_format_range range0_0_0;
} video_decode_capability_input = {
        .resp.hdr.type = VIRTIO_VIDEO_CMD_QUERY_CAPABILITY,
        .resp.hdr.stream_id = 0xffff,
        .resp.num_descs = 1,
        .desc0.mask = 0,
        .desc0.format = VIRTIO_VIDEO_FORMAT_H264,
        .desc0.planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER,
        .desc0.plane_align = 0,
        .desc0.num_frames = 1,
            .frame0_0.width.min = 0,
            .frame0_0.width.max = 640,
            .frame0_0.width.step = 64,
            .frame0_0.height.min = 0,
            .frame0_0.height.max = 480,
            .frame0_0.height.step = 48,
            .frame0_0.num_rates = 1,
                .range0_0_0.min = 60,
                .range0_0_0.max = 60,
                .range0_0_0.step = 1,
    };

static struct {
    struct virtio_video_query_capability_resp resp;
    /* Nested caps: desc->frame->range */
    struct virtio_video_format_desc desc0;
        struct virtio_video_format_frame frame0_0;
            struct virtio_video_format_range range0_0_0;
} video_decode_capability_output = {
        .resp.hdr.type = VIRTIO_VIDEO_CMD_QUERY_CAPABILITY,
        .resp.hdr.stream_id = 0xffff,
        .resp.num_descs = 1,
        .desc0.mask = 0,
        .desc0.format = VIRTIO_VIDEO_FORMAT_YUV420,
        .desc0.planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER,
        .desc0.plane_align = 0,
        .desc0.num_frames = 1,
            .frame0_0.width.min = 0,
            .frame0_0.width.max = 640,
            .frame0_0.width.step = 64,
            .frame0_0.height.min = 0,
            .frame0_0.height.max = 480,
            .frame0_0.height.step = 48,
            .frame0_0.num_rates = 1,
                .range0_0_0.min = 60,
                .range0_0_0.max = 60,
                .range0_0_0.step = 1,
    };

size_t virtio_video_dec_cmd_query_capability(VirtIODevice *vdev,
    virtio_video_query_capability *req, virtio_video_query_capability_resp **resp)
{
    size_t len = 0;
    void *src;
    VIRTVID_DEBUG("    %s: stream 0x%x, queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->queue_type);

    if (req != NULL && *resp == NULL) {
        switch (req->queue_type) {
        case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
            len = sizeof(video_decode_capability_input);
            src = &video_decode_capability_input;
            break;
        case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
            len = sizeof(video_decode_capability_output);
            src = &video_decode_capability_output;
            break;
        default:
            break;
        }

        *resp = g_malloc(len);
        if (*resp != NULL) {
            memcpy(*resp, src, len);
            (*resp)->hdr.type = req->hdr.type;
            (*resp)->hdr.stream_id = req->hdr.stream_id;
        } else {
            len = 0;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_get_params(VirtIODevice *vdev,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    size_t len = 0;

    VIRTVID_DEBUG("    %s: stream 0x%x, queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->queue_type);
    if (req != NULL && resp != NULL) {
        resp->hdr.type = req->hdr.type;
        resp->hdr.stream_id = req->hdr.stream_id;
        resp->params.queue_type = req->queue_type;
        len = sizeof(*resp);
    }

    return len;
}

size_t virtio_video_dec_event(VirtIODevice *vdev, virtio_video_event *ev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    size_t len = 0;

    if (ev) {
        ev->stream_id = ++vid->stream_id;
        len = sizeof(*ev);
        VIRTVID_DEBUG("    %s: event_type 0x%x, stream_id 0x%x", __FUNCTION__, ev->event_type, ev->stream_id);
    } else {
        VIRTVID_ERROR("Invalid virtio_video_event buffer");
    }

    return len;
}
