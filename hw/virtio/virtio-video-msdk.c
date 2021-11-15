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
#include "virtio-video-msdk.h"
#include "virtio-video-msdk-enc.h"
#include "virtio-video-msdk-dec.h"

size_t virtio_video_msdk_cmd_stream_create(VirtIOVideo *v,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_stream_create(v, req, resp);
    default:
        break;
    }

    return 0;
}

size_t virtio_video_msdk_cmd_stream_destroy(VirtIOVideo *v,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_stream_destroy(v, req, resp);
    default:
        break;
    }

    return 0;
}

size_t virtio_video_msdk_cmd_stream_drain(VirtIOVideo *v,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_stream_drain(v, req, resp);
    default:
        break;
    }

    return 0;
}

size_t virtio_video_msdk_cmd_resource_queue(VirtIOVideo *v,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_resource_queue(v, req, resp);
    default:
        break;
    }

    return 0;
}

size_t virtio_video_msdk_cmd_resource_destroy_all(VirtIOVideo *v,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_resource_destroy_all(v, req, resp);
    default:
        break;
    }

    return 0;
}

size_t virtio_video_msdk_cmd_queue_clear(VirtIOVideo *v,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_queue_clear(v, req, resp);
    default:
        break;
    }

    return 0;
}

size_t virtio_video_msdk_cmd_get_params(VirtIOVideo *v,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_get_params(v, req, resp);
    default:
        break;
    }

    return 0;
}

size_t virtio_video_msdk_cmd_set_params(VirtIOVideo *v,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_set_params(v, req, resp);
    default:
        break;
    }

    return 0;
}

size_t virtio_video_msdk_cmd_query_control(VirtIOVideo *v,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_query_control(v, req, resp);
    default:
        break;
    }

    return 0;
}

size_t virtio_video_msdk_cmd_get_control(VirtIOVideo *v,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_get_control(v, req, resp);
    default:
        break;
    }

    return 0;
}

size_t virtio_video_msdk_cmd_set_control(VirtIOVideo *v,
    virtio_video_set_control *req, virtio_video_set_control_resp *resp)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_msdk_dec_set_control(v, req, resp);
    default:
        break;
    }

    return 0;
}

int virtio_video_init_msdk(VirtIOVideo *v)
{
    int ret = -1;

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        ret = virtio_video_init_msdk_enc(v);
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        ret = virtio_video_init_msdk_dec(v);
        break;
    default:
        break;
    }

    return ret;
}

void virtio_video_uninit_msdk(VirtIOVideo *v)
{
    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        virtio_video_uninit_msdk_enc(v);
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        virtio_video_uninit_msdk_dec(v);
        break;
    default:
        break;
    }
}
