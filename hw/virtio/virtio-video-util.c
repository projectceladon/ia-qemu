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
#include "virtio-video-util.h"

struct virtio_video_event_bh_arg {
    VirtIODevice *vdev;
    uint32_t event_type;
    uint32_t stream_id;
};

int virtio_video_profile_range(uint32_t format, uint32_t *min, uint32_t *max)
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

int virtio_video_level_range(uint32_t format, uint32_t *min, uint32_t *max)
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

    resp.hdr.type = work->queue_type;
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

    g_free(work->elem);
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

static void virtio_video_cmd_others_complete(VirtIOVideoStream *stream,
                                             uint32_t cmd_type, bool success)
{
    VirtIOVideoCmd *cmd;
    VirtIOVideo *v = stream->parent;
    VirtIODevice *vdev = VIRTIO_DEVICE(v);
    virtio_video_cmd_hdr resp = {0};

    resp.type = success ? VIRTIO_VIDEO_RESP_OK_NODATA :
                          VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp.stream_id = stream->id;

    qemu_mutex_lock(&stream->mutex);
    QTAILQ_FOREACH(cmd, &stream->pending_cmds, next) {
        if (cmd->cmd_type == cmd_type)
            break;
    }
    if (cmd == NULL) {
        switch (cmd_type) {
        case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
            VIRTVID_ERROR("BUG: no pending stream drain request, but try to return");
            break;
        case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
            VIRTVID_ERROR("BUG: no pending queue clear request, but try to return");
            break;
        default:
            break;
        }
        qemu_mutex_unlock(&stream->mutex);
        return;
    }

    if (unlikely(iov_from_buf(cmd->elem->in_sg, cmd->elem->in_num, 0,
                              &resp, sizeof(resp)) != sizeof(resp))) {
        virtio_error(vdev, "virtio-video command response incorrect");
        virtqueue_detach_element(v->cmd_vq, cmd->elem, 0);
        goto done;
    }

    virtqueue_push(v->cmd_vq, cmd->elem, sizeof(resp));
    virtio_notify(vdev, v->cmd_vq);

done:
    QTAILQ_REMOVE(&stream->pending_cmds, cmd, next);
    g_free(cmd->elem);
    g_free(cmd);
    qemu_mutex_unlock(&stream->mutex);
}

static void virtio_video_stream_drain_done_bh(void *opaque)
{
    VirtIOVideoStream *stream = opaque;

    virtio_video_cmd_others_complete(stream, VIRTIO_VIDEO_CMD_STREAM_DRAIN, true);
}

static void virtio_video_stream_drain_failed_bh(void *opaque)
{
    VirtIOVideoStream *stream = opaque;

    virtio_video_cmd_others_complete(stream, VIRTIO_VIDEO_CMD_STREAM_DRAIN, false);
}

void virtio_video_stream_drain_done(VirtIOVideoStream *stream)
{
    VirtIOVideo *v = stream->parent;

    aio_bh_schedule_oneshot(v->ctx, virtio_video_stream_drain_done_bh, stream);
}

void virtio_video_stream_drain_failed(VirtIOVideoStream *stream)
{
    VirtIOVideo *v = stream->parent;

    aio_bh_schedule_oneshot(v->ctx, virtio_video_stream_drain_failed_bh, stream);
}

static void virtio_video_queue_clear_bh(void *opaque)
{
    VirtIOVideoStream *stream = opaque;

    virtio_video_cmd_others_complete(stream, VIRTIO_VIDEO_CMD_QUEUE_CLEAR, true);
}

void virtio_video_queue_clear_done(VirtIOVideoStream *stream)
{
    VirtIOVideo *v = stream->parent;

    aio_bh_schedule_oneshot(v->ctx, virtio_video_queue_clear_bh, stream);
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

void virtio_video_report_event(VirtIOVideo *v, uint32_t event, uint32_t stream_id)
{
    struct virtio_video_event_bh_arg *s;

    s = g_new0(struct virtio_video_event_bh_arg, 1);
    s->vdev = VIRTIO_DEVICE(v);
    s->event_type = event;
    s->stream_id = stream_id;

    aio_bh_schedule_oneshot(v->ctx, virtio_video_event_bh, s);
}
