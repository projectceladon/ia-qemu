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
#include "qemu/iov.h"
#include "qemu/error-report.h"
#include "virtio-video-util.h"

struct virtio_video_cmd_bh_arg {
    VirtIOVideo *v;
    VirtIOVideoCmd cmd;
    uint32_t stream_id;
};

struct virtio_video_work_bh_arg {
    VirtIOVideo *v;
    VirtIOVideoWork *work;
    uint32_t stream_id;
    uint32_t resource_id;
};

struct virtio_video_event_bh_arg {
    VirtIODevice *vdev;
    uint32_t event_type;
    uint32_t stream_id;
};

static struct {
    virtio_video_cmd_type cmd;
    const char *name;
} virtio_video_cmds[] = {
    {VIRTIO_VIDEO_CMD_QUERY_CAPABILITY, "QUERY_CAPABILITY"},
    {VIRTIO_VIDEO_CMD_STREAM_CREATE, "STREAM_CREATE"},
    {VIRTIO_VIDEO_CMD_STREAM_DESTROY, "STREAM_DESTROY"},
    {VIRTIO_VIDEO_CMD_STREAM_DRAIN, "STREAM_DRAIN"},
    {VIRTIO_VIDEO_CMD_RESOURCE_CREATE, "RESOURCE_CREATE"},
    {VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL, "RESOURCE_DESTROY_ALL"},
    {VIRTIO_VIDEO_CMD_RESOURCE_QUEUE, "RESOURCE_QUEUE"},
    {VIRTIO_VIDEO_CMD_QUEUE_CLEAR, "QUEUE_CLEAR"},
    {VIRTIO_VIDEO_CMD_GET_PARAMS, "GET_PARAMS"},
    {VIRTIO_VIDEO_CMD_SET_PARAMS, "SET_PARAMS"},
    {VIRTIO_VIDEO_CMD_QUERY_CONTROL, "QUERY_CONTROL"},
    {VIRTIO_VIDEO_CMD_GET_CONTROL, "GET_CONTROL"},
    {VIRTIO_VIDEO_CMD_SET_CONTROL, "SET_CONTROL"},
};

static struct {
    virtio_video_format format;
    const char *name;
} virtio_video_formats[] = {
    {VIRTIO_VIDEO_FORMAT_ARGB8888, "ARGB8"},
    {VIRTIO_VIDEO_FORMAT_BGRA8888, "BGRA8"},
    {VIRTIO_VIDEO_FORMAT_NV12, "NV12"},
    {VIRTIO_VIDEO_FORMAT_YUV420, "YUV420(IYUV)"},
    {VIRTIO_VIDEO_FORMAT_YVU420, "YVU420(YV12)"},

    {VIRTIO_VIDEO_FORMAT_MPEG2, "MPEG-2"},
    {VIRTIO_VIDEO_FORMAT_MPEG4, "MPEG-4"},
    {VIRTIO_VIDEO_FORMAT_H264, "H.264(AVC)"},
    {VIRTIO_VIDEO_FORMAT_HEVC, "H.265(HEVC)"},
    {VIRTIO_VIDEO_FORMAT_VP8, "VP8"},
    {VIRTIO_VIDEO_FORMAT_VP9, "VP9"},
};

const char *virtio_video_cmd_name(uint32_t cmd) {
    int i;

    for (i = 0; i < ARRAY_SIZE(virtio_video_cmds); i++) {
        if (virtio_video_cmds[i].cmd == cmd) {
            return virtio_video_cmds[i].name;
        }
    }
    return "UNKNOWN_CMD";
}

const char *virtio_video_format_name(uint32_t format) {
    int i;

    for (i = 0; i < ARRAY_SIZE(virtio_video_formats); i++) {
        if (virtio_video_formats[i].format == format) {
            return virtio_video_formats[i].name;
        }
    }
    return "UNKNOWN_FORMAT";
}

int virtio_video_format_profile_range(uint32_t format,
    uint32_t *min, uint32_t *max)
{
    if (min == NULL || max == NULL) {
        return -1;
    }

    switch (format) {
    case VIRTIO_VIDEO_FORMAT_H264:
        *min = VIRTIO_VIDEO_PROFILE_H264_MIN;
        *max = VIRTIO_VIDEO_PROFILE_H264_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
        *min = VIRTIO_VIDEO_PROFILE_HEVC_MIN;
        *max = VIRTIO_VIDEO_PROFILE_HEVC_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_VP8:
        *min = VIRTIO_VIDEO_PROFILE_VP8_MIN;
        *max = VIRTIO_VIDEO_PROFILE_VP8_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_VP9:
        *min = VIRTIO_VIDEO_PROFILE_VP9_MIN;
        *max = VIRTIO_VIDEO_PROFILE_VP9_MAX;
        break;
    default:
        return -1;
    }

    return 0;
}

int virtio_video_format_level_range(uint32_t format,
    uint32_t *min, uint32_t *max)
{
    if (min == NULL || max == NULL) {
        return -1;
    }

    switch (format) {
    case VIRTIO_VIDEO_FORMAT_H264:
        *min = VIRTIO_VIDEO_LEVEL_H264_MIN;
        *max = VIRTIO_VIDEO_LEVEL_H264_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
        *min = VIRTIO_VIDEO_LEVEL_HEVC_MIN;
        *max = VIRTIO_VIDEO_LEVEL_HEVC_MAX;
        break;
    default:
        return -1;
    }

    return 0;
}

bool virtio_video_format_is_codec(uint32_t format)
{
    switch (format) {
    case VIRTIO_VIDEO_FORMAT_ARGB8888:
    case VIRTIO_VIDEO_FORMAT_BGRA8888:
    case VIRTIO_VIDEO_FORMAT_NV12:
    case VIRTIO_VIDEO_FORMAT_YUV420:
    case VIRTIO_VIDEO_FORMAT_YVU420:
        return false;
    case VIRTIO_VIDEO_FORMAT_MPEG2:
    case VIRTIO_VIDEO_FORMAT_MPEG4:
    case VIRTIO_VIDEO_FORMAT_H264:
    case VIRTIO_VIDEO_FORMAT_HEVC:
    case VIRTIO_VIDEO_FORMAT_VP8:
    case VIRTIO_VIDEO_FORMAT_VP9:
        return true;
    default:
        return false;
    }
}

bool virtio_video_format_is_valid(uint32_t format, uint32_t num_planes)
{
    switch (format) {
    case VIRTIO_VIDEO_FORMAT_ARGB8888:
    case VIRTIO_VIDEO_FORMAT_BGRA8888:
        return num_planes == 1;
    case VIRTIO_VIDEO_FORMAT_NV12:
        return num_planes == 2;
    case VIRTIO_VIDEO_FORMAT_YUV420:
    case VIRTIO_VIDEO_FORMAT_YVU420:
        return num_planes == 3;
    case VIRTIO_VIDEO_FORMAT_MPEG2:
    case VIRTIO_VIDEO_FORMAT_MPEG4:
    case VIRTIO_VIDEO_FORMAT_H264:
    case VIRTIO_VIDEO_FORMAT_HEVC:
    case VIRTIO_VIDEO_FORMAT_VP8:
    case VIRTIO_VIDEO_FORMAT_VP9:
        /* multiplane for bitstream is undefined */
        return num_planes == 1;
    default:
        return false;
    }
}

bool virtio_video_param_fixup(virtio_video_params *params)
{
    switch (params->format) {
    case VIRTIO_VIDEO_FORMAT_ARGB8888:
    case VIRTIO_VIDEO_FORMAT_BGRA8888:
        if (params->num_planes == 1)
            break;
        params->num_planes = 1;
        params->plane_formats[0].plane_size =
            params->frame_width * params->frame_height * 4;
        printf("dyang23 %s:%d, plane_size:%d, wxh=%dx%d\n", __FUNCTION__, __LINE__, params->plane_formats[0].plane_size, params->frame_width, params->frame_height);
        params->plane_formats[0].stride = params->frame_width * 4;
        return true;
    case VIRTIO_VIDEO_FORMAT_NV12:
        if (params->num_planes == 2)
            break;
        params->num_planes = 2;
        params->plane_formats[0].plane_size =
            params->frame_width * params->frame_height;
        printf("dyang23 %s:%d, plane_size:%d, wxh=%dx%d\n", __FUNCTION__, __LINE__, params->plane_formats[0].plane_size, params->frame_width, params->frame_height);
        params->plane_formats[0].stride = params->frame_width;
        params->plane_formats[1].plane_size =
            params->frame_width * params->frame_height / 2;
        printf("dyang23 %s:%d, plane_size:%d, wxh=%dx%d\n", __FUNCTION__, __LINE__, params->plane_formats[1].plane_size, params->frame_width, params->frame_height);
        params->plane_formats[1].stride = params->frame_width;
        return true;
    case VIRTIO_VIDEO_FORMAT_YUV420:
    case VIRTIO_VIDEO_FORMAT_YVU420:
        if (params->num_planes == 3)
            break;
        params->num_planes = 3;
        params->plane_formats[0].plane_size =
            params->frame_width * params->frame_height;
        printf("dyang23 %s:%d, plane_size:%d, wxh=%dx%d\n", __FUNCTION__, __LINE__, params->plane_formats[0].plane_size, params->frame_width, params->frame_height);
        params->plane_formats[0].stride = params->frame_width;
        params->plane_formats[1].plane_size =
            params->frame_width * params->frame_height / 4;
        printf("dyang23 %s:%d, plane_size:%d, wxh=%dx%d\n", __FUNCTION__, __LINE__, params->plane_formats[1].plane_size, params->frame_width, params->frame_height);
        params->plane_formats[1].stride = params->frame_width / 2;
        params->plane_formats[2].plane_size =
            params->frame_width * params->frame_height / 4;
        printf("dyang23 %s:%d, plane_size:%d, wxh=%dx%d\n", __FUNCTION__, __LINE__, params->plane_formats[2].plane_size, params->frame_width, params->frame_height);
        params->plane_formats[2].stride = params->frame_width / 2;
        return true;
    case VIRTIO_VIDEO_FORMAT_MPEG2:
    case VIRTIO_VIDEO_FORMAT_MPEG4:
    case VIRTIO_VIDEO_FORMAT_H264:
    case VIRTIO_VIDEO_FORMAT_HEVC:
    case VIRTIO_VIDEO_FORMAT_VP8:
    case VIRTIO_VIDEO_FORMAT_VP9:
        /* multiplane for bitstream is undefined */
        if (params->num_planes == 1)
            break;
        params->num_planes = 1;
        return true;
    default:
        break;
    }

    return false;
}

void virtio_video_init_format(VirtIOVideoFormat *fmt, uint32_t format)
{
    if (fmt == NULL) {
        return;
    }

    QLIST_INIT(&fmt->frames);
    fmt->desc.mask = 0;
    fmt->desc.format = format;
    fmt->desc.planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER |
                              VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE;
    fmt->desc.plane_align = 0;
    fmt->desc.num_frames = 0;

    fmt->profile.num = 0;
    fmt->profile.values = NULL;
    fmt->level.num = 0;
    fmt->level.values = NULL;
}

static void virtio_video_destroy_resource(VirtIOVideoResource *resource,
    uint32_t mem_type, bool in)
{
    VirtIOVideoResourceSlice *slice;
    int i, j;

    QLIST_REMOVE(resource, next);
    for (i = 0; i < resource->num_planes; i++) {
        for (j = 0; j < resource->num_entries[i]; j++) {
            slice = &resource->slices[i][j];
            if (mem_type == VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES) {
                cpu_physical_memory_unmap(slice->page.hva, slice->page.len,
                                          !in, slice->page.len);
            } /* TODO: support object memory type */
        }
        g_free(resource->slices[i]);
    }
    g_free(resource);
}

void virtio_video_destroy_resource_list(VirtIOVideoStream *stream, bool in)
{
    VirtIOVideoResource *res, *tmp_res;
    virtio_video_mem_type mem_type;
    int dir;

    if (in) {
        mem_type = stream->in.mem_type;
        dir = VIRTIO_VIDEO_QUEUE_INPUT;
    } else {
        mem_type = stream->out.mem_type;
        dir = VIRTIO_VIDEO_QUEUE_OUTPUT;
    }

    QLIST_FOREACH_SAFE(res, &stream->resource_list[dir], next, tmp_res) {
        virtio_video_destroy_resource(res, mem_type, in);
    }
}

static int virtio_video_memcpy_singlebuffer(VirtIOVideoResource *res,
    uint32_t idx, void *src, uint32_t size)
{
    VirtIOVideoResourceSlice *slice;
    uint32_t begin = res->plane_offsets[idx], end = begin + size;
    uint32_t base = 0, diff, len;
    int i;

    for (i = 0; i < res->num_entries[0]; i++, base+= slice->page.len) {
        slice = &res->slices[0][i];
        if (begin >= base + slice->page.len)
            continue;
        /* begin >= base is always true */
        diff = begin - base;
        len = slice->page.len - diff;
        if (end <= base + slice->page.len) {
            memcpy(slice->page.hva + diff, src, size);
            return 0;
        } else {
            memcpy(slice->page.hva + diff, src, len);
            begin += len;
            size -= len;
            src += len;
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        //return -1;
    }

    return 0;
}

static int virtio_video_memcpy_perplane(VirtIOVideoResource *res,
    uint32_t idx, void *src, uint32_t size)
{
    VirtIOVideoResourceSlice *slice;
    int i;

    for (i = 0; i < res->num_entries[idx]; i++) {
        slice = &res->slices[idx][i];
        if (size <= slice->page.len) {
            memcpy(slice->page.hva, src, size);
            return 0;
        } else {
            memcpy(slice->page.hva, src, slice->page.len);
            size -= slice->page.len;
            src += slice->page.len;
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame, idx:%d, left size:%d", idx, size);
        //return -1;
    }

    return 0;
}

static int virtio_video_memdump_perplane(VirtIOVideoResource *res,
    uint32_t idx, void *dst, uint32_t size)
{
    VirtIOVideoResourceSlice *slice;
    int i;

    for (i = 0; i < res->num_entries[idx]; i++) {
        slice = &res->slices[idx][i];
        if (size <= slice->page.len) {
            memcpy(dst, slice->page.hva, size);
            return 0;
        } else {
            memcpy(dst, slice->page.hva, slice->page.len);
            size -= slice->page.len;
            dst += slice->page.len;
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        //return -1;
    }

    return 0;
}

int virtio_video_memdump(VirtIOVideoResource *res, uint32_t idx, void *dst,
    uint32_t size)
{
    switch (res->planes_layout) {
    case VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER:
        ;//return virtio_video_memcpy_singlebuffer(res, idx, dst, size);
    case VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE:
        return virtio_video_memdump_perplane(res, idx, dst, size);
    default:
        return -1;
    }
}

int virtio_video_memcpy(VirtIOVideoResource *res, uint32_t idx, void *src,
    uint32_t size)
{
    switch (res->planes_layout) {
    case VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER:
        return virtio_video_memcpy_singlebuffer(res, idx, src, size);
    case VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE:
        return virtio_video_memcpy_perplane(res, idx, src, size);
    default:
        return -1;
    }
}

static const char *virtio_video_event_name(uint32_t event)
{
    switch (event) {
    case VIRTIO_VIDEO_EVENT_ERROR:
        return "ERROR";
    case VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED:
        return "DECODER_RESOLUTION_CHANGED";
    default:
        return "UNKNOWN";
    }
}

/* @event must be removed from @event_queue first */
int virtio_video_event_complete(VirtIODevice *vdev, VirtIOVideoEvent *event)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    virtio_video_event resp = {0};

    resp.event_type = event->event_type;
    resp.stream_id = event->stream_id;

    if (unlikely(iov_from_buf(event->elem->in_sg, event->elem->in_num, 0,
                              &resp, sizeof(resp)) != sizeof(resp))) {
        virtio_error(vdev, "virtio-video event input incorrect");
        virtqueue_detach_element(v->event_vq, event->elem, 0);
        g_free(event->elem);
        g_free(event);
        return -1;
    }

    virtqueue_push(v->event_vq, event->elem, sizeof(resp));
    virtio_notify(vdev, v->event_vq);

    DPRINTF("stream %d event %s triggered\n", event->stream_id,
            virtio_video_event_name(resp.event_type));
    g_free(event->elem);
    g_free(event);
    return 0;
}

/*
 * Before the response of CMD_RESOURCE_QUEUE can be sent, these conditions must
 * be met:
 *  @work:           should be removed from @input_work or @output_work
 *  @work->resource: should be removed from @resource_list and destroyed
 */
static int virtio_video_cmd_resource_queue_complete(VirtIOVideo *v,
    VirtIOVideoWork *work, uint32_t stream_id, uint32_t resource_id)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(v);
    virtio_video_resource_queue_resp resp = {0};

    resp.hdr.type = VIRTIO_VIDEO_RESP_OK_NODATA;
    resp.hdr.stream_id = stream_id;
    resp.timestamp = work->timestamp;
    resp.flags = work->flags;
    resp.size = work->size;

    if (unlikely(iov_from_buf(work->elem->in_sg, work->elem->in_num, 0,
                              &resp, sizeof(resp)) != sizeof(resp))) {
        virtio_error(vdev, "virtio-video command response incorrect");
        virtqueue_detach_element(v->cmd_vq, work->elem, 0);
        g_free(work->elem);
        g_free(work);
        return -1;
    }

    virtqueue_push(v->cmd_vq, work->elem, sizeof(resp));
    virtio_notify(vdev, v->cmd_vq);

    DPRINTF("CMD_RESOURCE_DEQUEUE: stream %d dequeued %s resource %d, "
            "flags=0x%x size=%d\n", stream_id, work->queue_type ==
            VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? "input" : "output",
            resource_id, work->flags, work->size);
    g_free(work->elem);
    g_free(work);
    return 0;
}

static void virtio_video_output_one_work_bh(void *opaque)
{
    struct virtio_video_work_bh_arg *s = opaque;

    virtio_video_cmd_resource_queue_complete(s->v, s->work, s->stream_id,
                                             s->resource_id);
    object_unref(OBJECT(s->v));
    g_free(opaque);
}

/* must be called with stream->mutex held */
void virtio_video_work_done(VirtIOVideoWork *work)
{
    struct virtio_video_work_bh_arg *s;
    VirtIOVideoStream *stream = work->parent;
    VirtIOVideo *v = stream->parent;
    virtio_video_mem_type mem_type;

    s = g_new0(struct virtio_video_work_bh_arg, 1);
    s->v = v;
    s->work = work;
    s->stream_id = stream->id;
    s->resource_id = work->resource->id;

    if (work->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
        mem_type = stream->in.mem_type;
        virtio_video_destroy_resource(work->resource, mem_type, true);
    } else {
        mem_type = stream->out.mem_type;
        //virtio_video_destroy_resource(work->resource, mem_type, false);
    }

    object_ref(OBJECT(v));
    aio_bh_schedule_oneshot(v->ctx, virtio_video_output_one_work_bh, s);
}

static void virtio_video_cmd_others_complete(struct virtio_video_cmd_bh_arg *s,
    bool success)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s->v);
    virtio_video_cmd_hdr resp = {0};

    resp.type = success ? VIRTIO_VIDEO_RESP_OK_NODATA :
                          VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp.stream_id = s->stream_id;

    if (unlikely(iov_from_buf(s->cmd.elem->in_sg, s->cmd.elem->in_num, 0,
                              &resp, sizeof(resp)) != sizeof(resp))) {
        virtio_error(vdev, "virtio-video command response incorrect");
        virtqueue_detach_element(s->v->cmd_vq, s->cmd.elem, 0);
        g_free(s->cmd.elem);
        return;
    }

    virtqueue_push(s->v->cmd_vq, s->cmd.elem, sizeof(resp));
    virtio_notify(vdev, s->v->cmd_vq);

    switch (s->cmd.cmd_type) {
    case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
        DPRINTF("CMD_STREAM_DRAIN (async) for stream %d %s\n",
                s->stream_id, success ? "done" : "cancelled");
        break;
    case VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL:
        DPRINTF("CMD_RESOURCE_DESTROY_ALL (async) for stream %d %s\n",
                s->stream_id, success ? "done" : "cancelled");
        break;
    case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
        DPRINTF("CMD_QUEUE_CLEAR (async) for stream %d %s\n",
                s->stream_id, success ? "done" : "cancelled");
        break;
    case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
        DPRINTF("CMD_STREAM_DESTROY (async) for stream %d %s\n",
                s->stream_id, success ? "done" : "cancelled");
        break;
    default:
        break;
    }
    g_free(s->cmd.elem);
    object_unref(OBJECT(s->v));
}

static void virtio_video_cmd_done_bh(void *opaque)
{
    struct virtio_video_cmd_bh_arg *s = opaque;

    virtio_video_cmd_others_complete(s, true);
    g_free(opaque);
}

static void virtio_video_cmd_cancel_bh(void *opaque)
{
    struct virtio_video_cmd_bh_arg *s = opaque;

    virtio_video_cmd_others_complete(s, false);
    g_free(opaque);
}

void virtio_video_inflight_cmd_done(VirtIOVideoStream *stream)
{
    struct virtio_video_cmd_bh_arg *s;
    VirtIOVideo *v = stream->parent;

    s = g_new0(struct virtio_video_cmd_bh_arg, 1);
    s->v = v;
    s->cmd = stream->inflight_cmd;
    s->stream_id = stream->id;
    stream->inflight_cmd.cmd_type = 0;

    object_ref(OBJECT(v));
    aio_bh_schedule_oneshot(v->ctx, virtio_video_cmd_done_bh, s);
}

void virtio_video_inflight_cmd_cancel(VirtIOVideoStream *stream)
{
    struct virtio_video_cmd_bh_arg *s;
    VirtIOVideo *v = stream->parent;

    s = g_new0(struct virtio_video_cmd_bh_arg, 1);
    s->v = v;
    s->cmd = stream->inflight_cmd;
    s->stream_id = stream->id;
    stream->inflight_cmd.cmd_type = 0;

    object_ref(OBJECT(v));
    aio_bh_schedule_oneshot(v->ctx, virtio_video_cmd_cancel_bh, s);
}

static void virtio_video_event_bh(void *opaque)
{
    struct virtio_video_event_bh_arg *s = opaque;
    VirtIODevice *vdev = s->vdev;
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoEvent *event;

    qemu_mutex_lock(&v->mutex);

    event = QTAILQ_FIRST(&v->event_queue);
    if (event && event->elem) {
        event->event_type = s->event_type;
        event->stream_id = s->stream_id;
        QTAILQ_REMOVE(&v->event_queue, event, next);
        virtio_video_event_complete(vdev, event);
        goto done;
    }

    event = g_new0(VirtIOVideoEvent, 1);
    event->event_type = s->event_type;
    event->stream_id = s->stream_id;
    QTAILQ_INSERT_TAIL(&v->event_queue, event, next);

done:
    qemu_mutex_unlock(&v->mutex);
    object_unref(OBJECT(v));
    g_free(opaque);
}

void virtio_video_report_event(VirtIOVideo *v, uint32_t event,
    uint32_t stream_id)
{
    struct virtio_video_event_bh_arg *s;

    s = g_new0(struct virtio_video_event_bh_arg, 1);
    s->vdev = VIRTIO_DEVICE(v);
    s->event_type = event;
    s->stream_id = stream_id;

    object_ref(OBJECT(v));
    aio_bh_schedule_oneshot(v->ctx, virtio_video_event_bh, s);
}
