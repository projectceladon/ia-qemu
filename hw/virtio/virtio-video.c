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
#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "hw/virtio/virtio-video.h"
#include "virtio-video-dec.h"
#include "virtio-video-enc.h"

static struct {
    virtio_video_device_model id;
    const char *name;
} virtio_video_models[] = {
    {VIRTIO_VIDEO_DEVICE_V4L2_DEC, "v4l2-dec"},
    {VIRTIO_VIDEO_DEVICE_V4L2_ENC, "v4l2-enc"},
};

static struct {
    virtio_video_backend id;
    const char *name;
} virtio_video_backends[] = {
    {VIRTIO_VIDEO_BACKEND_VAAPI, "vaapi"},
    {VIRTIO_VIDEO_BACKEND_FFMPEG, "ffmpeg"},
    {VIRTIO_VIDEO_BACKEND_GSTREAMER, "gstreamer"},
    {VIRTIO_VIDEO_BACKEND_MEDIA_SDK, "media-sdk"},
};

static size_t virtio_video_process_cmd_query_capability(VirtIODevice *vdev,
    virtio_video_query_capability *req, virtio_video_query_capability_resp **resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoFormat *fmt;
    VirtIOVideoFormatFrame *fmt_frame;
    int num_descs = 0, idx;
    size_t len = sizeof(virtio_video_query_capability_resp);
    void *buf;

    if (req == NULL || *resp != NULL)
        return 0;
    VIRTVID_DEBUG("    %s: stream 0x%x, queue_type 0x%x", __FUNCTION__,
            req->hdr.stream_id, req->queue_type);

    switch(req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        idx = VIRTIO_VIDEO_FORMAT_LIST_INPUT;
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        idx = VIRTIO_VIDEO_FORMAT_LIST_OUTPUT;
        break;
    default:
        /* The request is invalid, respond with an error */
        *resp = g_malloc0(sizeof(virtio_video_cmd_hdr));
        if (*resp == NULL)
            return 0;
        ((virtio_video_cmd_hdr *)(*resp))->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
        ((virtio_video_cmd_hdr *)(*resp))->stream_id = req->hdr.stream_id;
        return sizeof(virtio_video_cmd_hdr);
    }

    QLIST_FOREACH(fmt, &v->format_list[idx], next) {
        num_descs++;
        len += sizeof(fmt->desc);
        QLIST_FOREACH(fmt_frame, &fmt->frames, next) {
            len += sizeof(fmt_frame->frame) + fmt_frame->frame.num_rates *
                   sizeof(virtio_video_format_range);
        }
    }
    *resp = g_malloc0(len);
    if (*resp == NULL)
        return 0;

    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY;
    (*resp)->hdr.stream_id = req->hdr.stream_id;
    (*resp)->num_descs = num_descs;

    buf = (char *)(*resp) + sizeof(virtio_video_query_capability_resp);
    QLIST_FOREACH(fmt, &v->format_list[idx], next) {
        memcpy(buf, &fmt->desc, sizeof(fmt->desc));
        buf += sizeof(fmt->desc);
        QLIST_FOREACH(fmt_frame, &fmt->frames, next) {
            memcpy(buf, &fmt_frame->frame, sizeof(fmt_frame->frame));
            buf += sizeof(fmt_frame->frame);
            for (idx = 0; idx < fmt_frame->frame.num_rates; idx++) {
                memcpy(buf, &fmt_frame->frame_rates[idx],
                       sizeof(virtio_video_format_range));
                buf += sizeof(virtio_video_format_range);
            }
        }
    }

    return len;
}

static size_t virtio_video_process_cmd_stream_create(VirtIODevice *vdev,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_stream_create(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_stream_destroy(VirtIODevice *vdev,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_stream_destroy(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_stream_drain(VirtIODevice *vdev,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_stream_drain(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_resource_create(VirtIODevice *vdev,
    virtio_video_resource_create *req, virtio_video_mem_entry *entries, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_resource_create(vdev, req, entries, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_resource_queue(VirtIODevice *vdev,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_resource_queue(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_resource_destroy_all(VirtIODevice *vdev,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_resource_destroy_all(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_queue_clear(VirtIODevice *vdev,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_queue_clear(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_get_params(VirtIODevice *vdev,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    if (req == NULL || resp == NULL)
        return 0;

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_get_params(vdev, req, resp);
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        return virtio_video_enc_cmd_get_params(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_set_params(VirtIODevice *vdev,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    if (req == NULL || resp == NULL)
        return 0;

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_set_params(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_query_control(VirtIODevice *vdev,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    if (req == NULL || *resp != NULL)
        return 0;

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_query_control(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_get_control(VirtIODevice *vdev,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    if (req == NULL || *resp != NULL)
        return 0;

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_get_control(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static size_t virtio_video_process_cmd_set_control(VirtIODevice *vdev,
    virtio_video_set_control *req, virtio_video_set_control_resp *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    if (req == NULL || resp == NULL)
        return 0;

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        return virtio_video_dec_cmd_set_control(vdev, req, resp);
        break;
    default:
        VIRTVID_ERROR("%s: Unknown virtio-device model %d", __FUNCTION__, v->model);
        return 0;
    }
}

static int virtio_video_process_command(VirtIODevice *vdev, struct iovec *in_buf,
    unsigned int in_num, struct iovec *out_buf, unsigned int out_num, size_t *size)
{
    virtio_video_cmd_hdr hdr = {0};
    size_t len = 0;

    if (size == NULL) {
        VIRTVID_ERROR("Invalid length buf processing command");
        return -1;
    }
    *size = 0;

    if (out_buf == NULL || out_num == 0) {
        VIRTVID_ERROR("Invalid out_buf(%p), out_num(%x) in cmd_vq", out_buf, out_num);
        return -1;
    }

    if (in_buf == NULL || in_num != 1) {
        VIRTVID_ERROR("Invalid in_buf(%p), in_num(%x) in cmd_vq", in_buf, in_num);
        return -1;
    }

    if (unlikely(iov_to_buf(out_buf, out_num, 0, &hdr, sizeof(hdr)) != sizeof(hdr))) {
        virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
        return -1;
    }

    VIRTVID_DEBUG("cmd 0x%x, stream 0x%x", hdr.type, hdr.stream_id);
    switch (hdr.type) {
    case VIRTIO_VIDEO_CMD_QUERY_CAPABILITY:
    {
        virtio_video_query_capability req = {0};
        virtio_video_query_capability_resp *resp = NULL;

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    queue_type 0x%x", req.queue_type);

        len = virtio_video_process_cmd_query_capability(vdev, &req, &resp);

        if (len == 0 || resp == NULL) {
            virtio_error(vdev, "virtio-video unexpected error while processing cmd_vq\n");
            return -1;
        }
        if (unlikely(iov_from_buf(in_buf, in_num, 0, resp, len) != len)) {
            g_free(resp);
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        g_free(resp);
        break;
    }
    case VIRTIO_VIDEO_CMD_STREAM_CREATE:
    {
        virtio_video_stream_create req = {0};
        virtio_video_cmd_hdr resp = {0};

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    in_mem_type 0x%x, out_mem_type 0x%x, coded_format 0x%x, tag %s",
                        req.in_mem_type, req.out_mem_type, req.coded_format, req.tag);

        len = virtio_video_process_cmd_stream_create(vdev, &req, &resp);
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        break;
    }
    case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
    {
        virtio_video_stream_destroy req = {0};
        virtio_video_cmd_hdr resp = {0};

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }

        len = virtio_video_process_cmd_stream_destroy(vdev, &req, &resp);
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        break;
    }
    case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
    {
        virtio_video_stream_drain req = {0};
        virtio_video_cmd_hdr resp = {0};

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }

        len = virtio_video_process_cmd_stream_drain(vdev, &req, &resp);
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        break;
    }
    case VIRTIO_VIDEO_CMD_RESOURCE_CREATE:
    {
        virtio_video_resource_create req = {0};
        virtio_video_mem_entry *entries = NULL;
        virtio_video_cmd_hdr resp = {0};
        size_t num_entries = 0;
        int i;

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    queue_type 0x%x, resource_id 0x%x, planes_layout 0x%x, num_planes 0x%x",
                        req.queue_type, req.resource_id, req.planes_layout, req.num_planes);

        for (i = 0; i < req.num_planes; i++) {
            num_entries += req.num_entries[i];
        }
        entries = g_malloc(sizeof(virtio_video_mem_entry) * num_entries);
        if (unlikely(iov_to_buf(out_buf, out_num, 1, entries,
                                sizeof(virtio_video_mem_entry) * num_entries) !=
                     sizeof(virtio_video_mem_entry) * num_entries)) {
            return -1;
        }

        len = virtio_video_process_cmd_resource_create(vdev, &req, entries, &resp);
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        break;
    }
    case VIRTIO_VIDEO_CMD_RESOURCE_QUEUE:
    {
        virtio_video_resource_queue req = {0};
        virtio_video_resource_queue_resp resp = {0};

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    queue_type 0x%x, resource_id 0x%x, timestamp 0x%llx, num_data_sizes 0x%x",
                        req.queue_type, req.resource_id, req.timestamp, req.num_data_sizes);

        len = virtio_video_process_cmd_resource_queue(vdev, &req, &resp);
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        break;
    }
    case VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL:
    {
        virtio_video_resource_destroy_all req = {0};
        virtio_video_cmd_hdr resp = {0};

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    queue_type 0x%x", req.queue_type);

        len = virtio_video_process_cmd_resource_destroy_all(vdev, &req, &resp);
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        break;
    }
    case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
    {
        virtio_video_queue_clear req = {0};
        virtio_video_cmd_hdr resp = {0};

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    queue_type 0x%x", req.queue_type);

        len = virtio_video_process_cmd_queue_clear(vdev, &req, &resp);
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        break;
    }
    case VIRTIO_VIDEO_CMD_GET_PARAMS:
    {
        virtio_video_get_params req = {0};
        virtio_video_get_params_resp resp = {0};

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    queue_type 0x%x", req.queue_type);

        len = virtio_video_process_cmd_get_params(vdev, &req, &resp);
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        break;
    }
    case VIRTIO_VIDEO_CMD_SET_PARAMS:
    {
        virtio_video_set_params req = {0};
        virtio_video_cmd_hdr resp = {0};

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    queue_type 0x%x", req.params.queue_type);

        len = virtio_video_process_cmd_set_params(vdev, &req, &resp);
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        break;
    }
    case VIRTIO_VIDEO_CMD_QUERY_CONTROL:
    {
        virtio_video_query_control req = {0};
        virtio_video_query_control_resp *resp = NULL;

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    control 0x%x", req.control);

        len = virtio_video_process_cmd_query_control(vdev, &req, &resp);

        if (len == 0 || resp == NULL) {
            virtio_error(vdev, "virtio-video unexpected error while processing cmd_vq\n");
            return -1;
        }
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            g_free(resp);
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        g_free(resp);
        break;
    }
    case VIRTIO_VIDEO_CMD_GET_CONTROL:
    {
        virtio_video_get_control req = {0};
        virtio_video_get_control_resp *resp = NULL;

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    control 0x%x", req.control);

        len = virtio_video_process_cmd_get_control(vdev, &req, &resp);
        if (len == 0 || resp == NULL) {
            virtio_error(vdev, "virtio-video unexpected error while processing cmd_vq\n");
            return -1;
        }
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            g_free(resp);
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        g_free(resp);
        break;
    }
    case VIRTIO_VIDEO_CMD_SET_CONTROL:
    {
        virtio_video_set_control req = {0};
        virtio_video_set_control_resp resp = {0};

        if (unlikely(iov_to_buf(out_buf, out_num, 0, &req, sizeof(req)) != sizeof(req))) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_to_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    control 0x%x", req.control);

        len = virtio_video_process_cmd_set_control(vdev, &req, &resp);
        if (unlikely(iov_from_buf(in_buf, in_num, 0, &resp, len) != len)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in cmd_vq\n");
            return -1;
        }
        VIRTVID_DEBUG("    resp_size 0x%lx", len);
        *size = len;
        break;
    }
    default:
        VIRTVID_ERROR("Unknown cmd 0x%x, stream 0x%x", hdr.type, hdr.stream_id);
        break;
    }

    return 0;
}

static void virtio_video_command_vq_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    if (!virtio_queue_ready(vq)) {
        VIRTVID_ERROR("cmd_vq isn't ready");
        return;
    }

    for (;;) {
        VirtQueueElement *elem;
        size_t len = 0;

        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem)
            break;

        VIRTVID_VERBOSE("cmd_vq index(%d) len(%d) ndescs(%d) out_num(%d) in_num(%d)",
                elem->index, elem->len, elem->ndescs, elem->out_num, elem->in_num);

        if ((elem->in_num == 1 && elem->out_num == 1) ||
            (elem->in_num == 1 && elem->out_num == 2)) {
            if (virtio_video_process_command(vdev, elem->in_sg, elem->in_num, elem->out_sg, elem->out_num, &len)) {
                virtqueue_detach_element(vq, elem, 0);
                g_free(elem);
                return;
            }

            virtqueue_push(vq, elem, len);
            virtio_notify(vdev, vq);
            g_free(elem);
        } else {
            virtio_error(vdev, "virtio-video unsupported buffer number in cmd_vq in(%d) out(%d)\n",
                         elem->in_num, elem->out_num);
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            return;
        }
    }
}

static void virtio_video_event_vq_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    if (!virtio_queue_ready(vq)) {
        VIRTVID_ERROR("event_vq isn't ready");
        return;
    }

    for (;;) {
        VirtIOVideoEvent *ev;
        VirtQueueElement *elem;

        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem)
            return;

        VIRTVID_VERBOSE("event_vq index(%d) len(%d) ndescs(%d) out_num(%d) in_num(%d)",
                elem->index, elem->len, elem->ndescs, elem->out_num, elem->in_num);

        if (elem->in_num != 1 || elem->out_num != 0) {
            virtio_error(vdev, "virtio-video unsupported buffer number in event_vq in(%d) out(%d)\n",
                         elem->in_num, elem->out_num);
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            return;
        }

        if (elem->in_sg[0].iov_len < sizeof(virtio_video_event)) {
            virtio_error(vdev, "virtio-video insufficient buffer for iov_from_buf in event_vq\n");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            return;
        }

        ev = g_malloc(sizeof(VirtIOVideoEvent));
        ev->elem = elem;
        QLIST_INSERT_HEAD(&v->event_list, ev, next);
    }
}

static void virtio_video_init_framework(VirtIODevice *vdev, Error **errp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    int i;

    if (!v->conf.model) {
        error_setg(errp, "virtio-video model isn't set");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(virtio_video_models); i++) {
        if (!strcmp(v->conf.model, virtio_video_models[i].name)) {
            v->model = virtio_video_models[i].id;
            break;
        }
    }
    if (i == ARRAY_SIZE(virtio_video_models)) {
        error_setg(errp, "Unknown virtio-video model %s", v->conf.model);
        return;
    }

    if (!v->conf.backend) {
        error_setg(errp, "virtio-video backend isn't set");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(virtio_video_backends); i++) {
        if (!strcmp(v->conf.backend, virtio_video_backends[i].name)) {
            v->backend = virtio_video_backends[i].id;
            break;
        }
    }
    if (i == ARRAY_SIZE(virtio_video_backends)) {
        error_setg(errp, "Unknown virtio-video backend %s", v->conf.backend);
        return;
    }

    VIRTVID_DEBUG("model %d(%s), backend %d(%s)", v->model, v->conf.model, v->backend, v->conf.backend);

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        virtio_init(vdev, "virtio-video", VIRTIO_ID_VIDEO_DEC, sizeof(virtio_video_config));
        if (virtio_video_decode_init(vdev)) {
            error_setg(errp, "Fail to initialize %s:%s", v->conf.model, v->conf.backend);
        }
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        virtio_init(vdev, "virtio-video", VIRTIO_ID_VIDEO_ENC, sizeof(virtio_video_config));
        if (virtio_video_encode_init(vdev)) {
            error_setg(errp, "Fail to initialize %s:%s", v->conf.model, v->conf.backend);
        }
        break;
    default:
        return;
    }

    v->cmd_vq = virtio_add_queue(vdev, VIRTIO_VIDEO_VQ_SIZE, virtio_video_command_vq_cb);
    if (v->cmd_vq == NULL) {
        error_setg(errp, "Fail to initialize virtio-video cmd_vq");
    }

    v->event_vq = virtio_add_queue(vdev, VIRTIO_VIDEO_VQ_SIZE, virtio_video_event_vq_cb);
    if (v->event_vq == NULL) {
        error_setg(errp, "Fail to initialize virtio-video event_vq");
    }
}

static void virtio_video_destroy_framework(VirtIODevice *vdev)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->model) {
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        virtio_video_decode_destroy(vdev);
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        virtio_video_encode_destroy(vdev);
        break;
    default:
        return;
    }

    virtio_del_queue(vdev, 0);
    virtio_del_queue(vdev, 1);
    virtio_cleanup(vdev);
}

static void virtio_video_init_internal(VirtIODevice *vdev, Error **errp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    v->config.version = VIRTIO_VIDEO_VERSION;
    v->config.max_caps_length = VIRTIO_VIDEO_CAPS_LENGTH_MAX;
    v->config.max_resp_length = VIRTIO_VIDEO_RESPONSE_LENGTH_MAX;
    QLIST_INIT(&v->event_list);
    QLIST_INIT(&v->stream_list);
    QLIST_INIT(&v->format_list[VIRTIO_VIDEO_FORMAT_LIST_INPUT]);
    QLIST_INIT(&v->format_list[VIRTIO_VIDEO_FORMAT_LIST_OUTPUT]);
}

static void virtio_video_destroy_format(VirtIOVideoFormat *fmt)
{
    VirtIOVideoFormatFrame *fmt_frame;

    QLIST_FOREACH(fmt_frame, &fmt->frames, next) {
        g_free(fmt_frame->frame_rates);
        QLIST_REMOVE(fmt_frame, next);
        g_free(fmt_frame);
    }
    QLIST_REMOVE(fmt, next);
    g_free(fmt);
}

static void virtio_video_destroy_internal(VirtIODevice *vdev)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoFormat *fmt;

    QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_FORMAT_LIST_INPUT], next) {
        virtio_video_destroy_format(fmt);
    }
    QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_FORMAT_LIST_OUTPUT], next) {
        virtio_video_destroy_format(fmt);
    }
}

static void virtio_video_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    Error *err = NULL;

    virtio_video_init_framework(vdev, &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }

    virtio_video_init_internal(vdev, &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }
}

static void virtio_video_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);

    virtio_video_destroy_internal(vdev);
    virtio_video_destroy_framework(vdev);
}

static void virtio_video_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    memcpy(config, &v->config, sizeof(v->config));
}

static void virtio_video_set_config(VirtIODevice *vdev, const uint8_t *config)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    memcpy(&v->config, config, sizeof(v->config));
}

static uint64_t virtio_video_get_features(VirtIODevice *vdev, uint64_t features,
                                          Error **errp)
{
    virtio_add_feature(&features, VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES);

    /* frontend only use one, either guest page or virtio object, guest page is prioritized */
    //virtio_add_feature(&features, VIRTIO_VIDEO_F_RESOURCE_VIRTIO_OBJECT);

    /* If support non-contiguous memory such as scatter-gather list */
    //virtio_add_feature(&features, VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG);
    return features;
}

static const VMStateDescription vmstate_virtio_video = {
    .name = "virtio-video",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static Property virtio_video_properties[] = {
    DEFINE_PROP_STRING("model", VirtIOVideo, conf.model),
    DEFINE_PROP_STRING("backend", VirtIOVideo, conf.backend),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_video_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_virtio_video;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, virtio_video_properties);
    vdc->realize = virtio_video_device_realize;
    vdc->unrealize = virtio_video_device_unrealize;
    vdc->get_config = virtio_video_get_config;
    vdc->set_config = virtio_video_set_config;
    vdc->get_features = virtio_video_get_features;
}

static const TypeInfo virtio_video_info = {
    .name          = TYPE_VIRTIO_VIDEO,
    .parent        = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOVideo),
    .class_init    = virtio_video_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_video_info);
}

type_init(virtio_register_types)

void virtio_video_resource_desc_from_guest_page(VirtIOVideoResourceDesc *desc)
{
    if (desc) {
        MemoryRegionSection section = memory_region_find(get_system_memory(), desc->entry.mem_entry.addr, desc->entry.mem_entry.length);

        desc->hva = memory_region_get_ram_ptr(section.mr) + section.offset_within_region;
        desc->len = desc->entry.mem_entry.length;
        desc->mr = section.mr;
    }
}
