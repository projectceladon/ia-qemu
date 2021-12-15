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
#include "virtio-video-util.h"
#include "virtio-video-msdk.h"
#include "virtio-video-msdk-dec.h"
#include "virtio-video-msdk-util.h"
#include "mfx/mfxvideo.h"

#define THREAD_NAME_LEN 24

static int virtio_video_decode_parse_header(VirtIOVideoWork *work)
{
    VirtIOVideoStream *stream = work->parent;
    MsdkSession *m_session = stream->opaque;
    MsdkFrame *m_frame = work->opaque;
    mfxStatus status;
    mfxVideoParam param = {0}, vpp_param = {0};
    mfxFrameAllocRequest alloc_req, vpp_req[2];

    memset(&alloc_req, 0, sizeof(alloc_req));
    memset(&vpp_req, 0, sizeof(alloc_req) * 2);

    virtio_video_msdk_load_plugin(m_session->session, stream->in.params.format, false);
    if (virtio_video_msdk_init_param_dec(&param, stream) < 0)
        return -1;

    /* TODO: handle MFX_ERR_MORE_DATA */
    status = MFXVideoDECODE_DecodeHeader(m_session->session, &m_frame->bitstream, &param);
    if (status != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_DecodeHeader failed with err %d",
                      stream->id, status);
        return -1;
    }

    status = MFXVideoDECODE_QueryIOSurf(m_session->session, &param, &alloc_req);
    if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_QueryIOSurf failed with err %d",
                      stream->id, status);
        return -1;
    }

    status = MFXVideoDECODE_Init(m_session->session, &param);
    if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_Init failed with err %d",
                      stream->id, status);
        return -1;
    }

    if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12)
        goto done;

    if (virtio_video_msdk_init_vpp_param_dec(&param, &vpp_param, stream) < 0)
        goto error_vpp;

    status = MFXVideoVPP_QueryIOSurf(m_session->session, &vpp_param, vpp_req);
    if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
        VIRTVID_ERROR("stream 0x%x MFXVideoVPP_QueryIOSurf failed with err %d",
                      stream->id, status);
        goto error_vpp;
    }

    status = MFXVideoVPP_Init(m_session->session, &vpp_param);
    if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
        VIRTVID_ERROR("stream 0x%x MFXVideoVPP_Init failed with err %d",
                      stream->id, status);
        goto error_vpp;
    }

    m_session->surface_num = vpp_req[0].NumFrameSuggested;
    m_session->vpp_surface_num = vpp_req[1].NumFrameSuggested;
    virtio_video_msdk_init_surface_pool(m_session, &vpp_req[1],
                                        &vpp_param.vpp.Out, true);

done:
    m_session->surface_num += alloc_req.NumFrameSuggested;
    virtio_video_msdk_init_surface_pool(m_session, &alloc_req,
                                        &param.mfx.FrameInfo, false);

    /* TODO: maybe we should keep crop values set by guest? */
    stream->out.params.min_buffers = alloc_req.NumFrameMin;
    stream->out.params.max_buffers = alloc_req.NumFrameSuggested;
    stream->out.params.frame_width = param.mfx.FrameInfo.Width;
    stream->out.params.frame_height = param.mfx.FrameInfo.Height;
    stream->out.params.frame_rate = param.mfx.FrameInfo.FrameRateExtN /
                                    param.mfx.FrameInfo.FrameRateExtD;
    stream->out.params.crop.left = param.mfx.FrameInfo.CropX;
    stream->out.params.crop.top = param.mfx.FrameInfo.CropY;
    stream->out.params.crop.width = param.mfx.FrameInfo.CropW;
    stream->out.params.crop.height = param.mfx.FrameInfo.CropH;

    stream->control.profile = virtio_video_msdk_to_profile(param.mfx.CodecProfile);
    stream->control.level = virtio_video_msdk_to_level(param.mfx.CodecLevel);
    return 0;

error_vpp:
    MFXVideoDECODE_Close(m_session->session);
    return -1;
}

static int virtio_video_decode_one_frame(VirtIOVideoWork *work)
{
    VirtIOVideoStream *stream = work->parent;
    MsdkSession *m_session = stream->opaque;
    MsdkFrame *m_frame = work->opaque;
    MsdkSurface *work_surface, *vpp_work_surface;
    mfxFrameSurface1 *out_surface = NULL;
    mfxStatus status;

    QLIST_FOREACH(work_surface, &m_session->surface_pool, next) {
        if (!work_surface->used && !work_surface->surface.Data.Locked) {
            break;
        }
    }
    if (work_surface == NULL) {
        VIRTVID_ERROR("No available surface in surface pool");
        return -1;
    }

    do {
        status = MFXVideoDECODE_DecodeFrameAsync(m_session->session,
                &m_frame->bitstream, &work_surface->surface, &out_surface,
                &m_frame->sync);
        switch (status) {
        case MFX_WRN_DEVICE_BUSY:
            usleep(1000);
            break;
        case MFX_ERR_NONE:
            break;
        /* TODO: support these */
        case MFX_ERR_MORE_SURFACE:
        case MFX_ERR_MORE_DATA:
        default:
            VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_DecodeFrameAsync failed with err %d",
                          stream->id, status);
            return -1;
        }
    } while (status == MFX_WRN_DEVICE_BUSY);

    QLIST_FOREACH(work_surface, &m_session->surface_pool, next) {
        if (&work_surface->surface == out_surface) {
            work_surface->used = true;
            m_frame->surface = work_surface;
            break;
        }
    }
    if (work_surface == NULL) {
        VIRTVID_ERROR("BUG: Decode output surface not in surface pool");
        return -1;
    }

    if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12)
        goto done;

    QLIST_FOREACH(vpp_work_surface, &m_session->vpp_surface_pool, next) {
        if (!vpp_work_surface->used && !vpp_work_surface->surface.Data.Locked) {
            work_surface->used = false;
            vpp_work_surface->used = true;
            m_frame->vpp_surface = vpp_work_surface;
            break;
        }
    }
    if (vpp_work_surface == NULL) {
        VIRTVID_ERROR("No available surface in surface pool");
        return -1;
    }

    do {
        status = MFXVideoVPP_RunFrameVPPAsync(m_session->session,
                &m_frame->surface->surface, &m_frame->vpp_surface->surface,
                NULL, &m_frame->vpp_sync);
        switch (status) {
        case MFX_WRN_DEVICE_BUSY:
            usleep(1000);
            break;
        case MFX_ERR_NONE:
            break;
        /* TODO: support these */
        case MFX_ERR_MORE_SURFACE:
        case MFX_ERR_MORE_DATA:
        default:
            VIRTVID_ERROR("stream 0x%x MFXVideoVPP_RunFrameVPPAsync failed with err %d",
                          stream->id, status);
            return -1;
        }
    } while (status == MFX_WRN_DEVICE_BUSY);

done:
    QTAILQ_REMOVE(&stream->pending_work, work, next);
    QTAILQ_INSERT_TAIL(&stream->queued_work, work, next);
    return 0;
}

static void virtio_video_decode_one_frame_bh(void *opaque)
{
    VirtIOVideoWork *work = opaque;
    VirtIOVideoStream *stream = work->parent;
    int ret;

    qemu_mutex_lock(&stream->mutex);

    ret = virtio_video_decode_one_frame(work);
    if (ret == 0) {
        qemu_mutex_unlock(&stream->mutex);
        return;
    }

    work->timestamp = 0;
    work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
    virtio_video_cmd_resource_queue_complete(work);

    qemu_mutex_unlock(&stream->mutex);
}

static void virtio_video_output_one_work_bh(void *opaque)
{
    VirtIOVideoWork *work = opaque;
    virtio_video_cmd_resource_queue_complete(work);
}

static void *virtio_video_decode_thread(void *arg)
{
    VirtIOVideoStream *stream = arg;
    VirtIOVideo *v = stream->parent;
    VirtIOVideoWork *work, *tmp_work;
    VirtIOVideoWork *work_out;
    MsdkSession *m_session = stream->opaque;
    MsdkFrame *m_frame;
    mfxStatus status;
    int ret;

    while (true) {
        qemu_mutex_lock(&stream->mutex);
        switch (stream->state) {
        case STREAM_STATE_INIT:
            qemu_mutex_unlock(&stream->mutex);

            /* Waiting for initial request which contains the header */
            qemu_event_wait(&m_session->notifier);
            qemu_event_reset(&m_session->notifier);
            continue;
        case STREAM_STATE_WAIT_METADATA:
            if (QTAILQ_EMPTY(&stream->pending_work)) {
                VIRTVID_ERROR("BUG: decode thread is woken up with empty input request queue");
                break;
            }

            work = QTAILQ_FIRST(&stream->pending_work);
            if (virtio_video_decode_parse_header(work) < 0) {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                QTAILQ_REMOVE(&stream->pending_work, work, next);
                aio_bh_schedule_oneshot(v->ctx, virtio_video_output_one_work_bh, work);
                break;
            }

            virtio_video_report_event(v, VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED, stream->id);

            /* initiate decoding for potentially multiple pending requests */
            QTAILQ_FOREACH_SAFE(work, &stream->pending_work, next, tmp_work) {
                if (virtio_video_decode_one_frame(work) < 0)
                    goto error;
            }

            stream->state = STREAM_STATE_RUNNING;
            break;
        case STREAM_STATE_RUNNING:
            QTAILQ_FOREACH(work, &stream->queued_work, next) {
                if (work->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                    m_frame = work->opaque;
                    break;
                }
            }
            if (work == NULL) {
                break;
            }

            QTAILQ_REMOVE(&stream->queued_work, work, next);
            qemu_mutex_unlock(&stream->mutex);

            if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12) {
                status = MFXVideoCORE_SyncOperation(m_session->session, m_frame->sync, MFX_INFINITE);
            } else {
                status = MFXVideoCORE_SyncOperation(m_session->session, m_frame->vpp_sync, MFX_INFINITE);
                if (status == MFX_ERR_ABORTED) {
                    status = MFXVideoCORE_SyncOperation(m_session->session, m_frame->sync, MFX_INFINITE);
                }
            }
            if (status != MFX_ERR_NONE) {
                VIRTVID_ERROR("stream 0x%x MFXVideoCORE_SyncOperation failed with err %d",
                              stream->id, status);
                goto error;
            }

            qemu_mutex_lock(&stream->mutex);
            QTAILQ_FOREACH(work_out, &stream->queued_work, next) {
                if (work_out->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                    break;
                }
            }
            if (work_out == NULL) {
                goto error;
            }

            if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12) {
                ret = virtio_video_msdk_output_surface(m_frame->surface, work_out->resource);
            } else {
                ret = virtio_video_msdk_output_surface(m_frame->vpp_surface, work_out->resource);
            }
            if (ret < 0) {
                goto error;
            }

            work_out->timestamp = work->timestamp;
            work->timestamp = 0;
            QTAILQ_REMOVE(&stream->queued_work, work_out, next);
            aio_bh_schedule_oneshot(v->ctx, virtio_video_output_one_work_bh, work);
            aio_bh_schedule_oneshot(v->ctx, virtio_video_output_one_work_bh, work_out);
            break;
        default:
            break;
        }
        qemu_mutex_unlock(&stream->mutex);
    }

done:
    if (stream->out.params.format != VIRTIO_VIDEO_FORMAT_NV12) {
        MFXVideoVPP_Close(m_session->session);
    }
    MFXVideoDECODE_Close(m_session->session);

    virtio_video_msdk_unload_plugin(m_session->session, stream->in.params.format, false);
    MFXClose(m_session->session);

    return NULL;

error:
    virtio_video_report_event(v, VIRTIO_VIDEO_EVENT_ERROR, stream->id);
    qemu_mutex_unlock(&stream->mutex);
    goto done;
}

size_t virtio_video_msdk_dec_stream_create(VirtIOVideo *v,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideoFormat *fmt;
    VirtIOVideoStream *stream;
    MsdkHandle *m_handle = v->opaque;
    MsdkSession *m_session;
    mfxStatus status;
    char thread_name[THREAD_NAME_LEN];
    size_t len;
    int i;

    mfxInitParam param = {
        .Implementation = MFX_IMPL_AUTO_ANY,
        .Version.Major = VIRTIO_VIDEO_MSDK_VERSION_MAJOR,
        .Version.Minor = VIRTIO_VIDEO_MSDK_VERSION_MINOR,
    };

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == resp->stream_id) {
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
            VIRTVID_ERROR("    %s: stream id 0x%x is already used", __func__,
                    resp->stream_id);
            return len;
        }
    }

    if (req->in_mem_type == VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT ||
            req->out_mem_type == VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT) {
        VIRTVID_ERROR("    %s: stream 0x%x, unsupported mem type (object)",
                __func__, resp->stream_id);
        return len;
    }

    QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_FORMAT_LIST_INPUT], next) {
        if (fmt->desc.format == req->coded_format) {
            break;
        }
    }
    if (fmt == NULL) {
        VIRTVID_ERROR("    %s: stream 0x%x, unsupported format 0x%x", __func__,
                resp->stream_id, req->coded_format);
        return len;
    }

    m_session = g_new0(MsdkSession, 1);
    if (m_session == NULL)
        return len;

    status = MFXInitEx(param, &m_session->session);
    if (status != MFX_ERR_NONE) {
        VIRTVID_ERROR("    %s: MFXInitEx returns %d for stream 0x%x", __func__,
                status, resp->stream_id);
        g_free(m_session);
        return len;
    }

    status = MFXVideoCORE_SetHandle(m_session->session, MFX_HANDLE_VA_DISPLAY,
                                    m_handle->va_handle);
    if (status != MFX_ERR_NONE) {
        VIRTVID_ERROR("    %s: MFXVideoCORE_SetHandle returns %d for stream 0x%x",
                __func__, status, resp->stream_id);
        MFXClose(m_session->session);
        g_free(m_session);
        return len;
    }

    stream = g_new0(VirtIOVideoStream, 1);
    if (stream == NULL) {
        g_free(m_session);
        return len;
    }
    stream->opaque = m_session;
    stream->parent = v;

    stream->id = req->hdr.stream_id;
    stream->in.mem_type = req->in_mem_type;
    stream->out.mem_type = req->out_mem_type;
    memcpy(stream->tag, req->tag, strlen((char *)req->tag));

    /*
     * The input of decode device is a bitstream. Frame rate, frame size, plane
     * formats and cropping rectangle are all meaningless, and will not be used
     * by frontend, so just use 0 as default value.
     */
    stream->in.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    stream->in.params.format = req->coded_format;
    stream->in.params.min_buffers = 1;
    stream->in.params.max_buffers = 32;
    stream->in.params.frame_rate = 0;
    stream->in.params.frame_width = 0;
    stream->in.params.frame_height = 0;
    stream->in.params.crop.left = 0;
    stream->in.params.crop.top = 0;
    stream->in.params.crop.width = 0;
    stream->in.params.crop.height = 0;
    stream->in.params.num_planes = 1;
    stream->in.params.plane_formats[0].plane_size = 0;
    stream->in.params.plane_formats[0].stride = 0;

    /**
     * The output of decode device is frames of some pixel format. We choose
     * NV12 as the default format but this can be changed by frontend.
     *
     * @format:                     the default output format of MediaSDK is
     *                              NV12, to support other formats we need to
     *                              convert from NV12 through VPP
     *
     * @frame_width, frame_height:  should be derived from input bitstream,
     *                              default value is just a placeholder
     *
     * @crop:                       currently treated as MediaSDK CropX/Y/W/H
     *                              values which are derived from input
     *                              bitstream, default value is just a
     *                              placeholder
     *
     *                              TODO: this can also mean cropping of output
     *                              frame, figure out the real meaning of this
     *                              field.
     *
     * @frame_rate:                 this field is only used by encode device,
     *                              the output frame rate can even be variable
     *                              (i.e. VFR), default value is just a
     *                              placeholder
     *
     * @num_planes, plane_formats:  use the parameters of NV12 format, this can
     *                              be changed later when frontend sets format
     *                              for device output.
     */
    stream->out.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    stream->out.params.format = VIRTIO_VIDEO_FORMAT_NV12;
    stream->out.params.min_buffers = 1;
    stream->out.params.max_buffers = 32;
    stream->out.params.frame_rate = fmt->frames.lh_first->frame_rates[0].max;
    stream->out.params.frame_width = fmt->frames.lh_first->frame.width.max;
    stream->out.params.frame_height = fmt->frames.lh_first->frame.height.max;
    stream->out.params.crop.left = 0;
    stream->out.params.crop.top = 0;
    stream->out.params.crop.width = stream->out.params.frame_width;
    stream->out.params.crop.height = stream->out.params.frame_height;
    stream->out.params.num_planes = 2;
    stream->out.params.plane_formats[0].plane_size =
        stream->out.params.frame_width * stream->out.params.frame_height;
    stream->out.params.plane_formats[0].stride = stream->out.params.frame_width;
    stream->out.params.plane_formats[1].plane_size =
        stream->out.params.frame_width * stream->out.params.frame_height / 2;
    stream->out.params.plane_formats[1].stride = stream->out.params.frame_width;

    /* Initialize control values */
    stream->control.bitrate = 0;
    stream->control.profile = 0;
    stream->control.level = 0;
    switch (req->coded_format) {
    case VIRTIO_VIDEO_FORMAT_H264:
        stream->control.profile = VIRTIO_VIDEO_PROFILE_H264_BASELINE;
        stream->control.level = VIRTIO_VIDEO_LEVEL_H264_1_0;
        break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
        stream->control.profile = VIRTIO_VIDEO_PROFILE_HEVC_MAIN;
        stream->control.level = VIRTIO_VIDEO_LEVEL_HEVC_1_0;
        break;
    case VIRTIO_VIDEO_FORMAT_VP8:
        stream->control.profile = VIRTIO_VIDEO_PROFILE_VP8_PROFILE0;
        break;
    case VIRTIO_VIDEO_FORMAT_VP9:
        stream->control.profile = VIRTIO_VIDEO_PROFILE_VP9_PROFILE0;
        break;
    default:
        break;
    }

    stream->state = STREAM_STATE_INIT;
    for (i = 0; i < VIRTIO_VIDEO_RESOURCE_LIST_NUM; i++) {
        QLIST_INIT(&stream->resource_list[i]);
    }
    QTAILQ_INIT(&stream->pending_work);
    QTAILQ_INIT(&stream->queued_work);
    qemu_mutex_init(&stream->mutex);

    qemu_event_init(&m_session->notifier, false);
    QLIST_INIT(&m_session->surface_pool);
    QLIST_INIT(&m_session->vpp_surface_pool);

    snprintf(thread_name, sizeof(thread_name), "virtio-video-decode/%d",
             stream->id);
    qemu_thread_create(&m_session->thread, thread_name, virtio_video_decode_thread,
                       stream, QEMU_THREAD_JOINABLE);

    QLIST_INSERT_HEAD(&v->stream_list, stream, next);
    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;

    VIRTVID_DEBUG("    %s: stream 0x%x created", __func__, stream->id);
    return len;
}

size_t virtio_video_msdk_dec_stream_destroy(VirtIOVideo *v,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp)
{
    MsdkSession *m_session;
    VirtIOVideoStream *stream, *next = NULL;
    VirtIOVideoResource *res, *next_res = NULL;
    size_t len = 0;
    int i;

    /* TODO: rewrite STREAM_DESTROY logic */
    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, next) {
        if (stream->id == req->hdr.stream_id) {
            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            m_session = stream->opaque;

            qemu_thread_join(&m_session->thread);
            m_session->thread.thread = 0;

            for (i = 0; i < VIRTIO_VIDEO_RESOURCE_LIST_NUM; i++) {
                QLIST_FOREACH_SAFE(res, &stream->resource_list[i], next, next_res) {
                    QLIST_SAFE_REMOVE(res, next);
                    g_free(res);
                }
            }

            qemu_mutex_destroy(&stream->mutex);
            g_free(m_session);
            QLIST_SAFE_REMOVE(stream, next);
            g_free(stream);
            VIRTVID_DEBUG("    %s: stream 0x%x destroyed", __func__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_msdk_dec_stream_drain(VirtIOVideo *v,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideoStream *stream;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        return len;
    }

    /* TODO: implement STREAM_DRAIN logic */
    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    return len;
}

size_t virtio_video_msdk_dec_resource_queue(VirtIOVideo *v,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp,
    VirtQueueElement *elem)
{
    MsdkSession *m_session;
    MsdkFrame *m_frame;
    VirtIOVideoStream *stream;
    VirtIOVideoResource *resource;
    VirtIOVideoWork *work;
    size_t len;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            m_session = stream->opaque;
            break;
        }
    }
    if (stream == NULL) {
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        return len;
    }

    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        QLIST_FOREACH(resource,
                &stream->resource_list[VIRTIO_VIDEO_RESOURCE_LIST_INPUT], next) {
            if (resource->id == req->resource_id) {
                break;
            }
        }
        if (resource == NULL) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
            return len;
        }

        if (req->num_data_sizes != 1) {
            VIRTVID_ERROR("    %s: stream 0x%x num_data_sizes=%d, should be 1 "
                    "for resources on input queue", __func__, stream->id, req->num_data_sizes);
            return len;
        }

        /* The same resource cannot be queued again if already queued. */
        qemu_mutex_lock(&stream->mutex);
        QTAILQ_FOREACH(work, &stream->pending_work, next) {
            if (resource->id == work->resource->id) {
                qemu_mutex_unlock(&stream->mutex);
                return len;
            }
        }
        QTAILQ_FOREACH(work, &stream->queued_work, next) {
            if (resource->id == work->resource->id) {
                qemu_mutex_unlock(&stream->mutex);
                return len;
            }
        }

        m_frame = g_new0(MsdkFrame, 1);
        m_frame->bitstream.Data = resource->slices[0]->page.hva;
        m_frame->bitstream.DataLength = req->data_sizes[0];
        m_frame->bitstream.MaxLength = req->data_sizes[0];

        work = g_new0(VirtIOVideoWork, 1);
        work->parent = stream;
        work->elem = elem;
        work->resource = resource;
        work->queue_type = req->queue_type;
        work->timestamp = req->timestamp;
        work->opaque = m_frame;

        /*
         * Input resources first go to the pending work list. After the
         * DecodeFrameAsync function is called for them, they are moved to the
         * queued work list.
         */
        QTAILQ_INSERT_TAIL(&stream->pending_work, work, next);
        switch (stream->state) {
        case STREAM_STATE_INIT:
            /* It is not allowed to change parameters from now on. */
            stream->state = STREAM_STATE_WAIT_METADATA;
            qemu_event_set(&m_session->notifier);
            break;
        case STREAM_STATE_WAIT_METADATA:
            /* Do not decode the frame in case the initialization is not done yet. */
            break;
        case STREAM_STATE_RUNNING:
            aio_bh_schedule_oneshot(v->ctx, virtio_video_decode_one_frame_bh, work);
            break;
        default:
            break;
        }
        qemu_mutex_unlock(&stream->mutex);

        len = 0;
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        QLIST_FOREACH(resource,
                &stream->resource_list[VIRTIO_VIDEO_RESOURCE_LIST_OUTPUT], next) {
            if (resource->id == req->resource_id) {
                break;
            }
        }
        if (resource == NULL) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }

        /* Only input resources have timestamp assigned. */
        if (req->timestamp != 0) {
            VIRTVID_WARN("    %s: stream 0x%x timestamp=%llu, should be 0 "
                    "for resources on output queue", __func__, stream->id, req->timestamp);
        }

        qemu_mutex_lock(&stream->mutex);
        QTAILQ_FOREACH(work, &stream->pending_work, next) {
            if (resource->id == work->resource->id) {
                qemu_mutex_unlock(&stream->mutex);
                return len;
            }
        }
        QTAILQ_FOREACH(work, &stream->queued_work, next) {
            if (resource->id == work->resource->id) {
                qemu_mutex_unlock(&stream->mutex);
                return len;
            }
        }

        work = g_new0(VirtIOVideoWork, 1);
        work->parent = stream;
        work->elem = elem;
        work->resource = resource;
        work->queue_type = req->queue_type;

        /*
         * Output resource is just a container for the decoded frame, it does
         * not involve submitting MediaSDK tasks.
         */
        QTAILQ_INSERT_TAIL(&stream->queued_work, work, next);
        qemu_mutex_unlock(&stream->mutex);

        len = 0;
        break;
    default:
        break;
    }

    return len;
}

size_t virtio_video_msdk_dec_resource_destroy_all(VirtIOVideo *v,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideoStream *stream;
    VirtIOVideoResource *res, *next_res = NULL;
    int i;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL)
        return len;

    /* TODO: reimplement RESOURCE_DESTROY_ALL logic */

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
        QLIST_FOREACH_SAFE(res,
                &stream->resource_list[VIRTIO_VIDEO_RESOURCE_LIST_INPUT],
                next, next_res) {
            for (i = 0; i < res->num_planes; i++) {
                g_free(res->slices[i]);
            }
            QLIST_REMOVE(res, next);
            g_free(res);
        }
    } else if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
        QLIST_FOREACH_SAFE(res,
                &stream->resource_list[VIRTIO_VIDEO_RESOURCE_LIST_OUTPUT],
                next, next_res) {
            for (i = 0; i < res->num_planes; i++) {
                g_free(res->slices[i]);
            }
            QLIST_REMOVE(res, next);
            g_free(res);
        }
    } else {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
        VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x",
                __func__, req->hdr.stream_id, req->queue_type);
    }
    VIRTVID_DEBUG("    %s: stream 0x%x queue_type 0x%x all resource destroyed",
            __func__, req->hdr.stream_id, req->queue_type);

    return len;
}

size_t virtio_video_msdk_dec_queue_clear(VirtIOVideo *v,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideoStream *stream;
    size_t len;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        return len;
    }

    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        break;
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
        VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x",
                __func__, req->hdr.stream_id, req->queue_type);
        return len;
    }

    /* TODO: implement QUEUE_CLEAR logic */
    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    return len;
}

size_t virtio_video_msdk_dec_get_params(VirtIOVideo *v,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    VirtIOVideoStream *stream;
    size_t len;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL)
        return len;

    resp->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_PARAMS;
    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        memcpy(&resp->params, &stream->in.params, sizeof(resp->params));
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        memcpy(&resp->params, &stream->out.params, sizeof(resp->params));
        break;
    default:
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
        break;
    }

    return len;
}

size_t virtio_video_msdk_dec_set_params(VirtIOVideo *v,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideoStream *stream;
    size_t len;
    int i;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL)
        return len;

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    switch (req->params.queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        if (stream->state == STREAM_STATE_INIT) {
            /*
             * The plane formats reflect frontend's organization of input
             * bitstream. It will then be read through CMD_GET_PARAMS in case
             * the backend does any adjustment, so just keep it.
             *
             * TODO: investigate if we should ignore it and just provide the
             * frontend a default `plane_size` instead of 0.
             */
            stream->in.params.format = req->params.format;
            stream->in.params.num_planes = req->params.num_planes;
            for (i = 0; i < req->params.num_planes; i++) {
                stream->in.params.plane_formats[i].plane_size =
                    req->params.plane_formats[i].plane_size;
                stream->in.params.plane_formats[i].stride =
                    req->params.plane_formats[i].stride;
            }
            if (req->params.num_planes > 1) {
                VIRTVID_WARN("    %s: stream 0x%x num_planes of input queue set to %d, should be 1",
                        __func__, stream->id, req->params.num_planes);
            }
        } else {
            VIRTVID_WARN("    %s: stream 0x%x not allowed to change param "
                         "after decoding has started", __func__, stream->id);
        }
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        /* Output parameters should be derived from input bitstream */
        /* TODO: figure out if we need to process output crop */
        if (stream->state == STREAM_STATE_INIT) {
            /* TODO: do we need to do sanity check? */
            stream->out.params.format = req->params.format;
            stream->out.params.num_planes = req->params.num_planes;
            for (i = 0; i < req->params.num_planes; i++) {
                stream->out.params.plane_formats[i].plane_size =
                    req->params.plane_formats[i].plane_size;
                stream->out.params.plane_formats[i].stride =
                    req->params.plane_formats[i].stride;
            }
        } else {
            VIRTVID_WARN("    %s: stream 0x%x not allowed to change param "
                         "after decoding has started", __func__, stream->id);
        }
        break;
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
        break;
    }

    return len;
}

size_t virtio_video_msdk_dec_query_control(VirtIOVideo *v,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp)
{
    VirtIOVideoFormat *fmt;
    void *req_buf = (char *)req + sizeof(virtio_video_query_control);
    void *resp_buf;
    size_t len = sizeof(virtio_video_query_control_resp);

    switch (req->control) {
    case VIRTIO_VIDEO_CONTROL_PROFILE:
    {
        virtio_video_query_control_profile *query = req_buf;
        virtio_video_query_control_resp_profile *resp_profile;

        QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_FORMAT_LIST_INPUT], next) {
            if (fmt->desc.format == query->format) {
                break;
            }
        }
        if (fmt == NULL) {
            VIRTVID_ERROR("    %s: unsupported format 0x%x", __func__, query->format);
            goto error;
        }
        if (fmt->profile.num == 0) {
            VIRTVID_ERROR("    %s: format 0x%x does not support profiles", __func__, query->format);
            goto error;
        }

        len += sizeof(virtio_video_query_control_resp_profile) +
               sizeof(uint32_t) * fmt->profile.num;
        *resp = g_malloc0(len);
        resp_profile = resp_buf = (char *)(*resp) + sizeof(virtio_video_query_control_resp);
        resp_profile->num = fmt->profile.num;
        resp_buf += sizeof(virtio_video_query_control_resp_profile);
        memcpy(resp_buf, fmt->profile.values, sizeof(uint32_t) * fmt->profile.num);

        VIRTVID_DEBUG("    %s: format 0x%x supports %d profiles", __func__,
                query->format, fmt->profile.num);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_LEVEL:
    {
        virtio_video_query_control_level *query = req_buf;
        virtio_video_query_control_resp_level *resp_level;

        QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_FORMAT_LIST_INPUT], next) {
            if (fmt->desc.format == query->format) {
                break;
            }
        }
        if (fmt == NULL) {
            VIRTVID_ERROR("    %s: unsupported format 0x%x", __func__, query->format);
            goto error;
        }
        if (fmt->level.num == 0) {
            VIRTVID_ERROR("    %s: format 0x%x does not support levels", __func__, query->format);
            goto error;
        }

        len += sizeof(virtio_video_query_control_resp_level) +
               sizeof(uint32_t) * fmt->level.num;
        *resp = g_malloc0(len);
        resp_level = resp_buf = (char *)(*resp) + sizeof(virtio_video_query_control_resp);
        resp_level->num = fmt->level.num;
        resp_buf += sizeof(virtio_video_query_control_resp_level);
        memcpy(resp_buf, fmt->level.values, sizeof(uint32_t) * fmt->level.num);

        VIRTVID_DEBUG("    %s: format 0x%x supports %d levels", __func__,
                query->format, fmt->level.num);
        break;
    }
    default:
        VIRTVID_ERROR("    %s: unsupported control %d", __func__, req->control);
        goto error;
    }

    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL;
    (*resp)->hdr.stream_id = req->hdr.stream_id;
    return len;

error:
    *resp = g_malloc(sizeof(virtio_video_query_control_resp));
    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
    (*resp)->hdr.stream_id = req->hdr.stream_id;
    return sizeof(virtio_video_query_control_resp);
}

size_t virtio_video_msdk_dec_get_control(VirtIOVideo *v,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp)
{
    VirtIOVideoStream *stream;
    size_t len = sizeof(virtio_video_get_control_resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        *resp = g_malloc(sizeof(virtio_video_get_control_resp));
        (*resp)->hdr.stream_id = req->hdr.stream_id;
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        return len;
    }

    switch (req->control) {
    case VIRTIO_VIDEO_CONTROL_BITRATE:
    {
        virtio_video_control_val_bitrate *val;

        if (stream->control.bitrate == 0)
            goto error;

        len += sizeof(virtio_video_control_val_bitrate);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(virtio_video_get_control_resp);
        val->bitrate = stream->control.bitrate;
        VIRTVID_DEBUG("    %s: stream 0x%x bitrate %d", __func__,
                req->hdr.stream_id, val->bitrate);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_PROFILE:
    {
        virtio_video_control_val_profile *val;

        if (stream->control.profile == 0)
            goto error;

        len += sizeof(virtio_video_control_val_profile);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(virtio_video_get_control_resp);
        val->profile = stream->control.profile;
        VIRTVID_DEBUG("    %s: stream 0x%x profile %d", __func__,
                req->hdr.stream_id, val->profile);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_LEVEL:
    {
        virtio_video_control_val_level *val;

        if (stream->control.level == 0)
            goto error;

        len += sizeof(virtio_video_control_val_level);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(virtio_video_get_control_resp);
        val->level = stream->control.level;
        VIRTVID_DEBUG("    %s: stream 0x%x level %d", __func__,
                req->hdr.stream_id, val->level);
        break;
    }
    default:
error:
        *resp = g_malloc(sizeof(virtio_video_get_control_resp));
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
        VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __func__,
                req->hdr.stream_id, req->control);
        break;
    }

    (*resp)->hdr.stream_id = req->hdr.stream_id;
    return len;
}

size_t virtio_video_msdk_dec_set_control(VirtIOVideo *v,
    virtio_video_set_control *req, virtio_video_set_control_resp *resp)
{
    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->hdr.stream_id = req->hdr.stream_id;
    VIRTVID_ERROR("    SET_CONTROL is not allowed in decoder");
    return sizeof(*resp);
}

int virtio_video_init_msdk_dec(VirtIOVideo *v)
{
    MsdkHandle *m_handle;
    VirtIOVideoFormat *in_fmt = NULL, *out_fmt = NULL;
    VirtIOVideoFormatFrame *in_fmt_frame = NULL, *out_fmt_frame = NULL;
    virtio_video_format in_format;
    virtio_video_format out_format[] = {VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_ARGB8888};
    mfxStatus status;
    mfxSession session;
    int i;

    mfxInitParam init_param = {
        .Implementation = MFX_IMPL_AUTO_ANY,
        .Version.Major = VIRTIO_VIDEO_MSDK_VERSION_MAJOR,
        .Version.Minor = VIRTIO_VIDEO_MSDK_VERSION_MINOR,
    };

    mfxVideoParam param = {0}, corrected_param = {0};

    if (virtio_video_msdk_init_handle(v)) {
        VIRTVID_ERROR("Fail to create VA environment on DRM");
        return -1;
    }

    status = MFXInitEx(init_param, &session);
    if (status != MFX_ERR_NONE) {
        VIRTVID_ERROR("MFXInitEx returns %d", status);
        return -1;
    }

    m_handle = v->opaque;
    status = MFXVideoCORE_SetHandle(session, MFX_HANDLE_VA_DISPLAY, m_handle->va_handle);
    if (status != MFX_ERR_NONE) {
        VIRTVID_ERROR("MFXVideoCORE_SetHandle returns %d", status);
        MFXClose(session);
        return -1;
    }

    for (in_format = VIRTIO_VIDEO_FORMAT_CODED_MIN;
            in_format <= VIRTIO_VIDEO_FORMAT_CODED_MAX; in_format++) {
        uint32_t w_min = 0, h_min = 0, w_max = 0, h_max = 0;
        uint32_t ctrl_min, ctrl_max, ctrl;
        uint32_t msdk_format = virtio_video_format_to_msdk(in_format);

        if (msdk_format == 0) {
            continue;
        }

        /* Check whether the format is supported */
        param.mfx.CodecId = msdk_format;
        status = MFXVideoDECODE_Query(session, NULL, &param);
        if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
            VIRTVID_DEBUG("format %x isn't supported by MSDK, status %d", in_format, status);
            continue;
        }

        virtio_video_msdk_load_plugin(session, in_format, false);
        virtio_video_msdk_init_param(&param, in_format);
        corrected_param.mfx.CodecId = param.mfx.CodecId;

        /* Query the max size supported */
        param.mfx.FrameInfo.Width = VIRTIO_VIDEO_MSDK_DIMENSION_MAX;
        param.mfx.FrameInfo.Height = VIRTIO_VIDEO_MSDK_DIMENSION_MAX;
        while (param.mfx.FrameInfo.Width >= VIRTIO_VIDEO_MSDK_DIMENSION_MIN &&
                param.mfx.FrameInfo.Height >= VIRTIO_VIDEO_MSDK_DIMENSION_MIN) {
            status = MFXVideoDECODE_Query(session, &param, &corrected_param);
            if (status == MFX_ERR_NONE || status == MFX_WRN_PARTIAL_ACCELERATION) {
                w_max = corrected_param.mfx.FrameInfo.Width;
                h_max = corrected_param.mfx.FrameInfo.Height;
                break;
            }
            param.mfx.FrameInfo.Width -= VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
            param.mfx.FrameInfo.Height -= (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
                VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS;
        }

        /* Query the min size supported */
        param.mfx.FrameInfo.Width = VIRTIO_VIDEO_MSDK_DIMENSION_MIN;
        param.mfx.FrameInfo.Height = (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
            VIRTIO_VIDEO_MSDK_DIMENSION_MIN : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS;
        while (param.mfx.FrameInfo.Width <= w_max && param.mfx.FrameInfo.Height <= h_max) {
            status = MFXVideoDECODE_Query(session, &param, &corrected_param);
            if (status == MFX_ERR_NONE || status == MFX_WRN_PARTIAL_ACCELERATION) {
                w_min = corrected_param.mfx.FrameInfo.Width;
                h_min = corrected_param.mfx.FrameInfo.Height;
                break;
            }
            param.mfx.FrameInfo.Width += VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
            param.mfx.FrameInfo.Height += (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
                VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS;
        }

        if (w_min == 0 || h_min == 0 || w_max == 0 || h_max == 0) {
            VIRTVID_DEBUG("failed to query frame size for format %x", in_format);
            goto out;
        }

        in_fmt = g_new0(VirtIOVideoFormat, 1);
        virtio_video_init_format(in_fmt, in_format);

        in_fmt_frame = g_new0(VirtIOVideoFormatFrame, 1);
        in_fmt_frame->frame.width.min = w_min;
        in_fmt_frame->frame.width.max = w_max;
        in_fmt_frame->frame.width.step = VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
        in_fmt_frame->frame.height.min = h_min;
        in_fmt_frame->frame.height.max = h_max;
        in_fmt_frame->frame.height.step = (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
            VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS;

        /* For decoding, frame rate may be unspecified */
        in_fmt_frame->frame.num_rates = 1;
        in_fmt_frame->frame_rates = g_new0(virtio_video_format_range, 1);
        in_fmt_frame->frame_rates[0].min = 1;
        in_fmt_frame->frame_rates[0].max = 60;
        in_fmt_frame->frame_rates[0].step = 1;

        in_fmt->desc.num_frames++;
        QLIST_INSERT_HEAD(&in_fmt->frames, in_fmt_frame, next);
        QLIST_INSERT_HEAD(&v->format_list[VIRTIO_VIDEO_FORMAT_LIST_INPUT], in_fmt, next);

        VIRTVID_DEBUG("Add input caps for format %x, width [%d, %d]@%d, "
                      "height [%d, %d]@%d, rate [%d, %d]@%d", in_format,
                      w_min, w_max, in_fmt_frame->frame.width.step,
                      h_min, h_max, in_fmt_frame->frame.height.step,
                      in_fmt_frame->frame_rates[0].min, in_fmt_frame->frame_rates[0].max,
                      in_fmt_frame->frame_rates[0].step);

        /* Query supported profiles */
        if (virtio_video_profile_range(in_format, &ctrl_min, &ctrl_max) < 0)
            goto out;
        in_fmt->profile.values = g_malloc0(sizeof(uint32_t) * (ctrl_max - ctrl_min) + 1);
        for (ctrl = ctrl_min; ctrl <= ctrl_max; ctrl++) {
            param.mfx.CodecProfile = virtio_video_profile_to_msdk(ctrl);
            if (param.mfx.CodecProfile == 0)
                continue;
            status = MFXVideoDECODE_Query(session, &param, &corrected_param);
            if (status == MFX_ERR_NONE || status == MFX_WRN_PARTIAL_ACCELERATION) {
                in_fmt->profile.values[in_fmt->profile.num++] = param.mfx.CodecProfile;
            }
        }
        if (in_fmt->profile.num == 0)
            g_free(in_fmt->profile.values);

        /* Query supported levels */
        if (virtio_video_level_range(in_format, &ctrl_min, &ctrl_max) < 0)
            goto out;
        in_fmt->level.values = g_malloc0(sizeof(uint32_t) * (ctrl_max - ctrl_min) + 1);
        param.mfx.CodecProfile = 0;
        for (ctrl = ctrl_min; ctrl <= ctrl_max; ctrl++) {
            param.mfx.CodecLevel = virtio_video_level_to_msdk(ctrl);
            if (param.mfx.CodecLevel == 0)
                continue;
            status = MFXVideoDECODE_Query(session, &param, &corrected_param);
            if (status == MFX_ERR_NONE || status == MFX_WRN_PARTIAL_ACCELERATION) {
                in_fmt->level.values[in_fmt->level.num++] = param.mfx.CodecLevel;
            }
        }
        if (in_fmt->level.num == 0)
            g_free(in_fmt->level.values);
out:
        virtio_video_msdk_unload_plugin(session, in_format, false);
    }

    /*
     * For Decoding, frame size/rate of output depends on input, so only one
     * dummy frame desc is required for each output format.
     */
    in_fmt = QLIST_FIRST(&v->format_list[VIRTIO_VIDEO_FORMAT_LIST_INPUT]);
    in_fmt_frame = QLIST_FIRST(&in_fmt->frames);
    for (i = 0; i < ARRAY_SIZE(out_format); i++) {
        size_t len;

        out_fmt = g_new0(VirtIOVideoFormat, 1);
        virtio_video_init_format(out_fmt, out_format[i]);

        out_fmt_frame = g_new0(VirtIOVideoFormatFrame, 1);
        memcpy(&out_fmt_frame->frame, &in_fmt_frame->frame, sizeof(virtio_video_format_frame));

        len = sizeof(virtio_video_format_range) * in_fmt_frame->frame.num_rates;
        out_fmt_frame->frame_rates = g_malloc0(len);
        memcpy(out_fmt_frame->frame_rates, in_fmt_frame->frame_rates, len);

        out_fmt->desc.num_frames++;
        QLIST_INSERT_HEAD(&out_fmt->frames, out_fmt_frame, next);
        QLIST_INSERT_HEAD(&v->format_list[VIRTIO_VIDEO_FORMAT_LIST_OUTPUT], out_fmt, next);

        VIRTVID_DEBUG("Add output caps for format %x, width [%d, %d]@%d, "
                      "height [%d, %d]@%d, rate [%d, %d]@%d", out_format[i],
                      out_fmt_frame->frame.width.min, out_fmt_frame->frame.width.max,
                      out_fmt_frame->frame.width.step, out_fmt_frame->frame.height.min,
                      out_fmt_frame->frame.height.max, out_fmt_frame->frame.height.step,
                      out_fmt_frame->frame_rates[0].min, out_fmt_frame->frame_rates[0].max,
                      out_fmt_frame->frame_rates[0].step);
    }

    QLIST_FOREACH(in_fmt, &v->format_list[VIRTIO_VIDEO_FORMAT_LIST_INPUT], next) {
        for (i = 0; i < ARRAY_SIZE(out_format); i++) {
            in_fmt->desc.mask |= BIT_ULL(i);
        }
    }

    MFXClose(session);

    return 0;
}

void virtio_video_uninit_msdk_dec(VirtIOVideo *v)
{
    VirtIOVideoStream *stream, *tmp_stream;

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, tmp_stream) {
        virtio_video_stream_destroy req = {0};
        virtio_video_cmd_hdr resp = {0};

        /* Destroy all in case CMD_STREAM_DESTROY not called on some stream */
        req.hdr.stream_id = stream->id;
        virtio_video_msdk_dec_stream_destroy(v, &req, &resp);
    }

    virtio_video_msdk_uninit_handle(v);
}
