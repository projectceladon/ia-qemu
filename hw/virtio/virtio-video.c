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
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "hw/virtio/virtio-video.h"
#include "virtio-video-msdk.h"

static struct {
    virtio_video_device_model id;
    const char *name;
} virtio_video_models[] = {
    {VIRTIO_VIDEO_DEVICE_V4L2_ENC, "v4l2-enc"},
    {VIRTIO_VIDEO_DEVICE_V4L2_DEC, "v4l2-dec"},
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

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_stream_create(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_stream_destroy(VirtIODevice *vdev,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_stream_destroy(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_stream_drain(VirtIODevice *vdev,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_stream_drain(v, req, resp, elem);
    default:
        return 0;
    }
}

static int virtio_video_process_cmd_resource_create(VirtIODevice *vdev,
    virtio_video_resource_create *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *stream;
    VirtIOVideoResource *res;
    virtio_video_mem_type mem_type;
    size_t len, num_entries = 0;
    int i, j, dir;

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    resp->stream_id = req->hdr.stream_id;

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        return 0;
    }

    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        mem_type = stream->in.mem_type;
        dir = VIRTIO_VIDEO_RESOURCE_LIST_INPUT;
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        mem_type = stream->out.mem_type;
        dir = VIRTIO_VIDEO_RESOURCE_LIST_OUTPUT;
        break;
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        return 0;
    }

    QLIST_FOREACH(res, &stream->resource_list[dir], next) {
        if (res->id == req->resource_id) {
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
            return 0;
        }
    }

    /* Frontend will not set planes_layout sometimes, so do not return an error. */
    if (req->planes_layout != VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER &&
            req->planes_layout != VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE) {
        VIRTVID_WARN("    %s: stream 0x%x, create resource with invalid planes layout 0x%x",
                __func__, stream->id, req->planes_layout);
    }

    for (i = 0; i < req->num_planes; i++) {
        num_entries += req->num_entries[i];
    }

    res = g_new0(VirtIOVideoResource, 1);
    res->id = req->resource_id;
    res->planes_layout = req->planes_layout;
    res->num_planes = req->num_planes;
    memcpy(&res->plane_offsets, &req->plane_offsets, sizeof(res->plane_offsets));
    memcpy(&res->num_entries, &req->num_entries, sizeof(res->num_entries));

    switch (mem_type) {
    case VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES:
    {
        virtio_video_mem_entry *entries = NULL;
        MemoryRegionSection section;

        len = sizeof(virtio_video_mem_entry) * num_entries;
        entries = g_malloc(len);
        if (unlikely(iov_to_buf(elem->out_sg, elem->out_num, 1, entries, len) != len)) {
            virtio_error(vdev, "virtio-video resource create data incorrect");
            g_free(entries);
            g_free(res);
            return -1;
        }

        for (i = 0, num_entries = 0; i < res->num_planes; i++) {
            res->slices[i] = g_new0(VirtIOVideoResourceSlice, res->num_entries[i]);
            for (j = 0; j < res->num_entries[i]; j++) {
                section = memory_region_find(get_system_memory(),
                                             entries[num_entries].addr,
                                             entries[num_entries].length);
                res->slices[i][j].page.hva = memory_region_get_ram_ptr(section.mr) +
                                             section.offset_within_region;
                res->slices[i][j].page.len = entries[num_entries].length;
                num_entries++;
            }
        }
        g_free(entries);
        break;
    }
    case VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT:
    {
        /* TODO: support object memory type */
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        VIRTVID_ERROR("%s: Unsupported memory type (object)", __func__);
        return 0;
    }
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        return 0;
    }

    QLIST_INSERT_HEAD(&stream->resource_list[dir], res, next);
    return 0;
}

static size_t virtio_video_process_cmd_resource_queue(VirtIODevice *vdev,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp,
    VirtQueueElement *elem)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_resource_queue(v, req, resp, elem);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_resource_destroy_all(VirtIODevice *vdev,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_resource_destroy_all(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_queue_clear(VirtIODevice *vdev,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_queue_clear(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_get_params(VirtIODevice *vdev,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_get_params(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_set_params(VirtIODevice *vdev,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_set_params(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_query_control(VirtIODevice *vdev,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_query_control(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_get_control(VirtIODevice *vdev,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_get_control(v, req, resp);
    default:
        return 0;
    }
}

static size_t virtio_video_process_cmd_set_control(VirtIODevice *vdev,
    virtio_video_set_control *req, virtio_video_set_control_resp *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        return virtio_video_msdk_cmd_set_control(v, req, resp);
    default:
        return 0;
    }
}

/*
 * Process the command requested without blocking. The responce will not be
 * ready if the requested operation is blocking. The command will be recorded
 * and complete asynchronously.
 *
 * @return - 0 when response is ready, 1 when response is not ready, negative
 * value on failure
 */
static int virtio_video_process_command(VirtIODevice *vdev,
    VirtQueueElement *elem, size_t *resp_size)
{
    virtio_video_cmd_hdr hdr = {0};
    size_t len = *resp_size = 0;
    bool async = false;

#define CMD_GET_REQ(req, len) do {                                          \
        if (unlikely(iov_to_buf(elem->out_sg, elem->out_num, 0,             \
                                req, len) != len)) {                        \
            virtio_error(vdev, "virtio-video command request incorrect");   \
            return -1;                                                      \
        }                                                                   \
    } while (0)
#define CMD_SET_RESP(resp, len, alloc) do {                                 \
        if (len == 0 || resp == NULL) {                                     \
            virtio_error(vdev, "virtio-video command unexpected error");    \
            return -1;                                                      \
        }                                                                   \
        if (unlikely(iov_from_buf(elem->in_sg, elem->in_num, 0,             \
                                  resp, len)!= len)) {                      \
            if (alloc) {                                                    \
                g_free(resp);                                               \
            }                                                               \
            virtio_error(vdev, "virtio-video command response incorrect");  \
            return -1;                                                      \
        }                                                                   \
    } while (0)

    CMD_GET_REQ(&hdr, sizeof(hdr));
    VIRTVID_DEBUG("cmd 0x%x, stream 0x%x", hdr.type, hdr.stream_id);

    switch (hdr.type) {
    case VIRTIO_VIDEO_CMD_QUERY_CAPABILITY:
    {
        virtio_video_query_capability req = {0};
        virtio_video_query_capability_resp *resp = NULL;

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    queue_type 0x%x", req.queue_type);

        len = virtio_video_process_cmd_query_capability(vdev, &req, &resp);
        CMD_SET_RESP(resp, len, true);
        g_free(resp);
        break;
    }
    case VIRTIO_VIDEO_CMD_STREAM_CREATE:
    {
        virtio_video_stream_create req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    in_mem_type 0x%x, out_mem_type 0x%x, coded_format 0x%x, tag %s",
                        req.in_mem_type, req.out_mem_type, req.coded_format, req.tag);

        len = virtio_video_process_cmd_stream_create(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
    {
        virtio_video_stream_destroy req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_stream_destroy(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
    {
        virtio_video_stream_drain req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        len = virtio_video_process_cmd_stream_drain(vdev, &req, &resp, elem);
        if (len == 0) {
            async = true;
            break;
        }
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_RESOURCE_CREATE:
    {
        virtio_video_resource_create req = {0};
        virtio_video_cmd_hdr resp = {0};

        if (elem->out_num < 2) {
            virtio_error(vdev, "virtio-video command missing headers");
            return -1;
        }

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    queue_type 0x%x, resource_id 0x%x, planes_layout 0x%x, num_planes 0x%x",
                        req.queue_type, req.resource_id, req.planes_layout, req.num_planes);

        if (virtio_video_process_cmd_resource_create(vdev, &req, &resp, elem) < 1)
            return -1;
        CMD_SET_RESP(&resp, sizeof(resp), false);
        break;
    }
    case VIRTIO_VIDEO_CMD_RESOURCE_QUEUE:
    {
        virtio_video_resource_queue req = {0};
        virtio_video_resource_queue_resp resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    queue_type 0x%x, resource_id 0x%x, timestamp 0x%llx, num_data_sizes 0x%x",
                        req.queue_type, req.resource_id, req.timestamp, req.num_data_sizes);

        len = virtio_video_process_cmd_resource_queue(vdev, &req, &resp, elem);
        if (len == 0) {
            async = true;
            break;
        }
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL:
    {
        virtio_video_resource_destroy_all req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    queue_type 0x%x", req.queue_type);

        len = virtio_video_process_cmd_resource_destroy_all(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
    {
        virtio_video_queue_clear req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    queue_type 0x%x", req.queue_type);

        len = virtio_video_process_cmd_queue_clear(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_GET_PARAMS:
    {
        virtio_video_get_params req = {0};
        virtio_video_get_params_resp resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    queue_type 0x%x", req.queue_type);

        len = virtio_video_process_cmd_get_params(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_SET_PARAMS:
    {
        virtio_video_set_params req = {0};
        virtio_video_cmd_hdr resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    queue_type 0x%x", req.params.queue_type);

        len = virtio_video_process_cmd_set_params(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    case VIRTIO_VIDEO_CMD_QUERY_CONTROL:
    {
        virtio_video_query_control req = {0};
        virtio_video_query_control_resp *resp = NULL;

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    control 0x%x", req.control);

        len = virtio_video_process_cmd_query_control(vdev, &req, &resp);
        CMD_SET_RESP(resp, len, true);
        g_free(resp);
        break;
    }
    case VIRTIO_VIDEO_CMD_GET_CONTROL:
    {
        virtio_video_get_control req = {0};
        virtio_video_get_control_resp *resp = NULL;

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    control 0x%x", req.control);

        len = virtio_video_process_cmd_get_control(vdev, &req, &resp);
        CMD_SET_RESP(resp, len, true);
        g_free(resp);
        break;
    }
    case VIRTIO_VIDEO_CMD_SET_CONTROL:
    {
        virtio_video_set_control req = {0};
        virtio_video_set_control_resp resp = {0};

        CMD_GET_REQ(&req, sizeof(req));
        VIRTVID_DEBUG("    control 0x%x", req.control);

        len = virtio_video_process_cmd_set_control(vdev, &req, &resp);
        CMD_SET_RESP(&resp, len, false);
        break;
    }
    default:
        VIRTVID_ERROR("Unknown cmd 0x%x, stream 0x%x", hdr.type, hdr.stream_id);
        break;
    }

    *resp_size = len;
    return async;
}

static void virtio_video_command_vq_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtQueueElement *elem;
    size_t len = 0;
    int ret;

    if (!virtio_queue_ready(vq)) {
        VIRTVID_ERROR("cmd_vq isn't ready");
        return;
    }

    for (;;) {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem)
            break;

        VIRTVID_VERBOSE("cmd_vq index(%d) len(%d) ndescs(%d) out_num(%d) in_num(%d)",
                elem->index, elem->len, elem->ndescs, elem->out_num, elem->in_num);

        if (elem->out_num < 1 || elem->in_num < 1) {
            virtio_error(vdev, "virtio-video command missing headers");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            return;
        }

        qemu_mutex_lock(&v->mutex);
        ret = virtio_video_process_command(vdev, elem, &len);
        qemu_mutex_unlock(&v->mutex);
        if (ret < 0) {
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            return;
        } else if (ret == 0) {
            virtqueue_push(vq, elem, len);
            virtio_notify(vdev, vq);
            g_free(elem);
        } /* or return asynchronously */
    }
}

static void virtio_video_event_bh(void *opaque)
{
    VirtIODevice *vdev = opaque;
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoEvent *ev;
    virtio_video_event resp = {0};

    qemu_mutex_lock(&v->mutex);

    if (QLIST_EMPTY(&v->event_list))
        goto done;

    QLIST_FOREACH(ev, &v->event_list, next) {
        if (ev->event_type != 0)
            break;
    }
    if (ev == NULL)
        goto done;

    QLIST_REMOVE(ev, next);
    resp.event_type = ev->event_type;
    resp.stream_id = ev->stream_id;

    if (unlikely(iov_from_buf(ev->elem->in_sg, ev->elem->in_num, 0,
                              &resp, sizeof(resp)) != sizeof(resp))) {
        virtio_error(vdev, "virtio-video event input incorrect");
        virtqueue_detach_element(v->event_vq, ev->elem, 0);
        g_free(ev->elem);
        g_free(ev);
        goto done;
    }

    virtqueue_push(v->event_vq, ev->elem, sizeof(resp));
    virtio_notify(vdev, v->event_vq);
    g_free(ev->elem);
    g_free(ev);

done:
    qemu_mutex_unlock(&v->mutex);
}

static void virtio_video_event_vq_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoEvent *ev;
    VirtQueueElement *elem;

    if (!virtio_queue_ready(vq)) {
        VIRTVID_ERROR("event_vq isn't ready");
        return;
    }

    for (;;) {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem)
            return;

        VIRTVID_VERBOSE("event_vq index(%d) len(%d) ndescs(%d) out_num(%d) in_num(%d)",
                elem->index, elem->len, elem->ndescs, elem->out_num, elem->in_num);

        if (elem->in_num < 1) {
            virtio_error(vdev, "virtio-video event missing input");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            return;
        }

        if (elem->in_sg[0].iov_len < sizeof(virtio_video_event)) {
            virtio_error(vdev, "virtio-video event input too short");
            virtqueue_detach_element(vq, elem, 0);
            g_free(elem);
            return;
        }

        /* event type = 0 means no event assigned yet */
        ev = g_new(VirtIOVideoEvent, 1);
        ev->elem = elem;
        ev->event_type = 0;
        ev->stream_id = 0;

        qemu_mutex_lock(&v->mutex);
        QLIST_INSERT_HEAD(&v->event_list, ev, next);
        qemu_mutex_unlock(&v->mutex);
    }
}

static void virtio_video_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    int i, ret = -1;

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
    case VIRTIO_VIDEO_DEVICE_V4L2_ENC:
        virtio_init(vdev, "virtio-video", VIRTIO_ID_VIDEO_ENC, sizeof(virtio_video_config));
        break;
    case VIRTIO_VIDEO_DEVICE_V4L2_DEC:
        virtio_init(vdev, "virtio-video", VIRTIO_ID_VIDEO_DEC, sizeof(virtio_video_config));
        break;
    default:
        return;
    }

    v->config.version = VIRTIO_VIDEO_VERSION;
    v->config.max_caps_length = VIRTIO_VIDEO_CAPS_LENGTH_MAX;
    v->config.max_resp_length = VIRTIO_VIDEO_RESPONSE_LENGTH_MAX;

    v->cmd_vq = virtio_add_queue(vdev, VIRTIO_VIDEO_VQ_SIZE, virtio_video_command_vq_cb);
    v->event_vq = virtio_add_queue(vdev, VIRTIO_VIDEO_VQ_SIZE, virtio_video_event_vq_cb);

    QLIST_INIT(&v->event_list);
    QLIST_INIT(&v->stream_list);
    for (i = 0; i < VIRTIO_VIDEO_FORMAT_LIST_NUM; i++)
        QLIST_INIT(&v->format_list[i]);

    qemu_mutex_init(&v->mutex);
    if (v->conf.iothread) {
        object_ref(OBJECT(v->conf.iothread));
        v->ctx = iothread_get_aio_context(v->conf.iothread);
    } else {
        v->ctx = qemu_get_aio_context();
    }
    v->event_bh = aio_bh_new(v->ctx, virtio_video_event_bh, vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        ret = virtio_video_init_msdk(v);
        break;
    default:
        break;
    }

    if (ret) {
        qemu_mutex_destroy(&v->mutex);
        qemu_bh_delete(v->event_bh);
        if (v->conf.iothread) {
            object_unref(OBJECT(v->conf.iothread));
        }
        virtio_del_queue(vdev, 0);
        virtio_del_queue(vdev, 1);
        virtio_cleanup(vdev);
        error_setg(errp, "Failed to initialize %s:%s", v->conf.model, v->conf.backend);
    }
}

static void virtio_video_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoEvent *ev, *tmp_ev;
    VirtIOVideoFormat *fmt, *tmp_fmt;
    VirtIOVideoFormatFrame *frame, *tmp_frame;
    int i;

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        virtio_video_uninit_msdk(v);
        break;
    default:
        break;
    }

    QLIST_FOREACH_SAFE(ev, &v->event_list, next, tmp_ev) {
        virtqueue_detach_element(v->event_vq, ev->elem, 0);
        g_free(ev->elem);
        g_free(ev);
    }

    for (i = 0; i < VIRTIO_VIDEO_FORMAT_LIST_NUM; i++) {
        QLIST_FOREACH_SAFE(fmt, &v->format_list[i], next, tmp_fmt) {
            QLIST_FOREACH_SAFE(frame, &fmt->frames, next, tmp_frame) {
                g_free(frame->frame_rates);
                g_free(frame);
            }
            if (fmt->profile.num)
                g_free(fmt->profile.values);
            if (fmt->level.num)
                g_free(fmt->level.values);
            g_free(fmt);
        }
    }

    qemu_mutex_destroy(&v->mutex);
    qemu_bh_delete(v->event_bh);
    if (v->conf.iothread) {
        object_unref(OBJECT(v->conf.iothread));
    }

    virtio_del_queue(vdev, 0);
    virtio_del_queue(vdev, 1);
    virtio_cleanup(vdev);
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
    DEFINE_PROP_LINK("iothread", VirtIOVideo, conf.iothread, TYPE_IOTHREAD,
                     IOThread *),
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
