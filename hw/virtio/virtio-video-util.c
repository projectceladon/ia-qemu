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
        params->plane_formats[0].stride = params->frame_width * 4;
        return true;
    case VIRTIO_VIDEO_FORMAT_NV12:
        if (params->num_planes == 2)
            break;
        params->num_planes = 2;
        params->plane_formats[0].plane_size =
            params->frame_width * params->frame_height;
        params->plane_formats[0].stride = params->frame_width;
        params->plane_formats[1].plane_size =
            params->frame_width * params->frame_height / 2;
        params->plane_formats[1].stride = params->frame_width;
        return true;
    case VIRTIO_VIDEO_FORMAT_YUV420:
    case VIRTIO_VIDEO_FORMAT_YVU420:
        if (params->num_planes == 3)
            break;
        params->num_planes = 3;
        params->plane_formats[0].plane_size =
            params->frame_width * params->frame_height;
        params->plane_formats[0].stride = params->frame_width;
        params->plane_formats[1].plane_size =
            params->frame_width * params->frame_height / 4;
        params->plane_formats[1].stride = params->frame_width / 2;
        params->plane_formats[2].plane_size =
            params->frame_width * params->frame_height / 4;
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

void virtio_video_destroy_resource_list(VirtIOVideoStream *stream, bool in)
{
    VirtIOVideoResource *res, *tmp_res;
    VirtIOVideoResourceSlice *slice;
    virtio_video_mem_type mem_type;
    int i, j, dir;

    if (in) {
        mem_type = stream->in.mem_type;
        dir = VIRTIO_VIDEO_QUEUE_INPUT;
    } else {
        mem_type = stream->out.mem_type;
        dir = VIRTIO_VIDEO_QUEUE_OUTPUT;
    }

    QLIST_FOREACH_SAFE(res, &stream->resource_list[dir], next, tmp_res) {
        QLIST_REMOVE(res, next);
        for (i = 0; i < res->num_planes; i++) {
            for (j = 0; j < res->num_entries[i]; j++) {
                slice = &res->slices[i][j];
                if (mem_type == VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES) {
                    cpu_physical_memory_unmap(slice->page.hva, slice->page.len,
                                              !in, slice->page.len);
                } /* TODO: support object memory type */
            }
            g_free(res->slices[i]);
        }
        g_free(res);
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
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        return -1;
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
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        return -1;
    }

    return 0;
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
 * @work must be removed from @pending_work, @input_work or @output_work first
 */
int virtio_video_cmd_resource_queue_complete(VirtIOVideoWork *work)
{
    VirtIOVideoStream *stream = work->parent;
    VirtIOVideo *v = stream->parent;
    VirtIODevice *vdev = VIRTIO_DEVICE(v);
    virtio_video_resource_queue_resp resp = {0};

    resp.hdr.type = VIRTIO_VIDEO_RESP_OK_NODATA;
    resp.hdr.stream_id = stream->id;
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

    DPRINTF("CMD_RESOURCE_QUEUE: stream %d dequeued %s resource %d "
            "timestamp 0x%lx flags 0x%x size %d\n",  stream->id,
            work->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ?
            "input" : "output", work->resource->id, work->timestamp,
            work->flags, work->size);
    g_free(work->elem);
    printf("dyang23, free work id:%d\n", work->id);
    g_free(work);
    return 0;
}

static void virtio_video_output_one_work_bh(void *opaque)
{
    VirtIOVideoWork *work = opaque;
    virtio_video_cmd_resource_queue_complete(work);
}

void virtio_video_work_done(VirtIOVideoWork *work)
{
    VirtIOVideoStream *stream = work->parent;
    VirtIOVideo *v = stream->parent;

    aio_bh_schedule_oneshot(v->ctx, virtio_video_output_one_work_bh, work);
}

static void virtio_video_cmd_others_complete(VirtIOVideoCmd *cmd, bool success)
{
    VirtIOVideoStream *stream = cmd->parent;
    VirtIOVideo *v = stream->parent;
    VirtIODevice *vdev = VIRTIO_DEVICE(v);
    virtio_video_cmd_hdr resp = {0};

    resp.type = success ? VIRTIO_VIDEO_RESP_OK_NODATA :
                          VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp.stream_id = stream->id;

    if (unlikely(iov_from_buf(cmd->elem->in_sg, cmd->elem->in_num, 0,
                              &resp, sizeof(resp)) != sizeof(resp))) {
        virtio_error(vdev, "virtio-video command response incorrect");
        virtqueue_detach_element(v->cmd_vq, cmd->elem, 0);
        goto done;
    }

    virtqueue_push(v->cmd_vq, cmd->elem, sizeof(resp));
    virtio_notify(vdev, v->cmd_vq);

    switch (cmd->cmd_type) {
    case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
        DPRINTF("CMD_STREAM_DRAIN (async) for stream %d %s\n",
                stream->id, success ? "done" : "cancelled");
        break;
    case VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL:
        DPRINTF("CMD_RESOURCE_DESTROY_ALL (async) for stream %d %s\n",
                stream->id, success ? "done" : "cancelled");
        break;
    case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
        DPRINTF("CMD_QUEUE_CLEAR (async) for stream %d %s\n",
                stream->id, success ? "done" : "cancelled");
        break;
    default:
        break;
    }

done:
    g_free(cmd->elem);
    g_free(cmd);
}

static void virtio_video_cmd_done_bh(void *opaque)
{
    VirtIOVideoCmd *cmd = opaque;

    virtio_video_cmd_others_complete(cmd, true);
}

static void virtio_video_cmd_cancel_bh(void *opaque)
{
    VirtIOVideoCmd *cmd = opaque;

    virtio_video_cmd_others_complete(cmd, false);
}

void virtio_video_cmd_done(VirtIOVideoCmd *cmd)
{
    VirtIOVideoStream *stream = cmd->parent;
    VirtIOVideo *v = stream->parent;

    aio_bh_schedule_oneshot(v->ctx, virtio_video_cmd_done_bh, cmd);
}

void virtio_video_cmd_cancel(VirtIOVideoCmd *cmd)
{
    VirtIOVideoStream *stream = cmd->parent;
    VirtIOVideo *v = stream->parent;

    aio_bh_schedule_oneshot(v->ctx, virtio_video_cmd_cancel_bh, cmd);
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

    aio_bh_schedule_oneshot(v->ctx, virtio_video_event_bh, s);
}
