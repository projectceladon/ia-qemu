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
#include "qemu/error-report.h"
#include "virtio-video-util.h"
#include "virtio-video-msdk.h"
#include "virtio-video-msdk-dec.h"
#include "virtio-video-msdk-util.h"
#include "mfx/mfxvideo.h"

#define THREAD_NAME_LEN 24

static bool first_frame_after_header = false;
static int virtio_video_decode_parse_header(VirtIOVideoWork *work)
{
    VirtIOVideoStream *stream = work->parent;
    MsdkSession *m_session = stream->opaque;
    MsdkFrame *m_frame = work->opaque;
    mfxStatus status;
    mfxVideoParam param = {0}, vpp_param = {0};
    mfxFrameAllocRequest alloc_req, vpp_req[2];
    int needed_length;
    unsigned char *new_data;
    printf("virtio_video_decode_parse_header\n");

    memset(&alloc_req, 0, sizeof(alloc_req));
    memset(&vpp_req, 0, sizeof(alloc_req) * 2);
    work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;

    virtio_video_msdk_load_plugin(m_session->session, stream->in.params.format, false);
    if (virtio_video_msdk_init_param_dec(&param, stream) < 0)
        return -1;

    if (stream->bitstream_header.Data == NULL) {
        stream->bitstream_header.Data = g_malloc0(m_frame->bitstream.MaxLength);
        memcpy(stream->bitstream_header.Data, m_frame->bitstream.Data, m_frame->bitstream.MaxLength);
        stream->bitstream_header.DataOffset = m_frame->bitstream.DataOffset;
        stream->bitstream_header.DataLength = m_frame->bitstream.DataLength;
        stream->bitstream_header.MaxLength = m_frame->bitstream.MaxLength;
    } else {
        needed_length = stream->bitstream_header.MaxLength + m_frame->bitstream.MaxLength;
        new_data = realloc(stream->bitstream_header.Data, needed_length);
        if (new_data) {
            stream->bitstream_header.Data = new_data;
            memcpy(stream->bitstream_header.Data + stream->bitstream_header.MaxLength, m_frame->bitstream.Data, m_frame->bitstream.MaxLength);
            stream->bitstream_header.DataLength += m_frame->bitstream.DataLength;
            stream->bitstream_header.MaxLength = needed_length;
        }
    }
    stream->bitstream_header.DataFlag |= MFX_BITSTREAM_COMPLETE_FRAME;

    status = MFXVideoDECODE_DecodeHeader(m_session->session,
                                         &stream->bitstream_header, &param);
    if (status != MFX_ERR_NONE) {
        if (status == MFX_ERR_MORE_DATA || status == MFX_ERR_NULL_PTR) {
            work->flags = 0;
        } else {
            error_report("virtio-video-decode/%d MFXVideoDECODE_DecodeHeader "
                         "failed: %d", stream->id, status);
        }
        return -1;
    }
    printf("bitstream Header decode success!\n");
    printf_mfxVideoParam(&param);
    first_frame_after_header = true;

    status = MFXVideoDECODE_QueryIOSurf(m_session->session, &param, &alloc_req);
    if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
        error_report("virtio-video-decode/%d MFXVideoDECODE_QueryIOSurf "
                      "failed: %d", stream->id, status);
        return -1;
    }

    status = MFXVideoDECODE_Init(m_session->session, &param);
    if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
        error_report("virtio-video-decode/%d MFXVideoDECODE_Init "
                      "failed: %d", stream->id, status);
        return -1;
    }

    if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12)
        goto done;

#if 1
    goto done;
    // VPP will be enabled later
    if (virtio_video_msdk_init_vpp_param_dec(&param, &vpp_param, stream) < 0)
        goto error_vpp;

    status = MFXVideoVPP_QueryIOSurf(m_session->session, &vpp_param, vpp_req);
    if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
        error_report("virtio-video-decode/%d MFXVideoVPP_QueryIOSurf "
                      "failed: %d", stream->id, status);
        goto error_vpp;
    }

    status = MFXVideoVPP_Init(m_session->session, &vpp_param);
    if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
        error_report("virtio-video-decode/%d MFXVideoVPP_Init "
                      "failed: %d", stream->id, status);
        goto error_vpp;
    }

    m_session->surface_num = vpp_req[0].NumFrameSuggested;
    m_session->vpp_surface_num = vpp_req[1].NumFrameSuggested;
    virtio_video_msdk_init_surface_pool(m_session, &vpp_req[1],
                                        &vpp_param.vpp.Out, true);
#endif
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
    work->flags = 0;
    return 0;

error_vpp:
    MFXVideoDECODE_Close(m_session->session);
    return -1;
}

static int virtio_video_decode_submit_one_frame(VirtIOVideoWork *work)
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
        error_report("virtio-video: stream %d no available surface "
                     "in surface pool", stream->id);
        return -1;
    }

    printf_mfxFrameSurface1(work_surface->surface);
    m_frame->bitstream.DataFlag |= MFX_BITSTREAM_COMPLETE_FRAME;

    do {
        
        printf_mfxBitstream(&m_frame->bitstream);
        if (first_frame_after_header) {
            printf_mfxBitstream(&stream->bitstream_header);
            status = MFXVideoDECODE_DecodeFrameAsync(m_session->session,
                    &stream->bitstream_header, &work_surface->surface, &out_surface,
                    &m_frame->sync);
            first_frame_after_header = false;
        }
        else {
            printf_mfxBitstream(&m_frame->bitstream);
            status = MFXVideoDECODE_DecodeFrameAsync(m_session->session,
                    &m_frame->bitstream, &work_surface->surface, &out_surface,
                    &m_frame->sync);
        }

        printf("dyang23, MFXVideoDECODE_DecodeFrameAsync return %d, out_surface:%p\n", status, out_surface);
        switch (status) {
        case MFX_WRN_DEVICE_BUSY:
            usleep(1000);
            break;
        case MFX_ERR_NONE:
            break;
        /* TODO: support these */
        case MFX_ERR_MORE_SURFACE:
        case MFX_ERR_MORE_DATA:
        /* More DATA&Surface should not be taken as error */
            break;
        default:
            error_report("virtio-video: stream %d input resource %d "
                         "MFXVideoDECODE_DecodeFrameAsync failed: %d",
                         stream->id, work->resource->id, status);
            return -1;
        }
    } while (status == MFX_WRN_DEVICE_BUSY);

    work_surface->used = true;
    if (status == MFX_ERR_MORE_DATA)
        return MFX_ERR_MORE_DATA;

    QLIST_FOREACH(work_surface, &m_session->surface_pool, next) {
        if (&work_surface->surface == out_surface) {
            //work_surface->used = true;
            m_frame->surface = work_surface;
            break;
        }
    }
    if (work_surface == NULL) {
        error_report("virtio-video: BUG: stream %d decode output surface "
                      "not in surface pool", stream->id);
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
        error_report("virtio-video: stream %d no available surface "
                     "in vpp surface pool", stream->id);
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
            error_report("virtio-video: stream %d input resource %d "
                         "MFXVideoVPP_RunFrameVPPAsync failed: %d",
                         stream->id, work->resource->id, status);
            return -1;
        }
    } while (status == MFX_WRN_DEVICE_BUSY);

done:
    QTAILQ_REMOVE(&stream->pending_work, work, next);
    QTAILQ_INSERT_TAIL(&stream->input_work, work, next);
    qemu_event_set(&m_session->notifier);
    return 0;
}

static void virtio_video_decode_submit_one_frame_bh(void *opaque)
{
    VirtIOVideoWork *work = opaque;
    VirtIOVideoStream *stream = work->parent;
    int ret;

    qemu_mutex_lock(&stream->mutex);

    DPRINTF("stream %d input resource %d start to decode\n",
            stream->id, work->resource->id);
    ret = virtio_video_decode_submit_one_frame(work);
    if (ret == 0) {
        qemu_mutex_unlock(&stream->mutex);
        return;
    }

    DPRINTF("stream %d input resource %d failed to start decoding\n",
            stream->id, work->resource->id);
    if (ret != MFX_ERR_MORE_DATA) {
        work->timestamp = 0;
        work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
    }
    QTAILQ_REMOVE(&stream->pending_work, work, next);
    g_free(work->opaque);
    virtio_video_cmd_resource_queue_complete(work);

    qemu_mutex_unlock(&stream->mutex);
}

static int virtio_video_decode_retrieve_one_frame(VirtIOVideoWork *work_in,
    VirtIOVideoWork *work_out)
{
    VirtIOVideoStream *stream = work_in->parent;
    MsdkSession *m_session = stream->opaque;
    MsdkFrame *m_frame = work_in->opaque;
    mfxStatus status;
    uint32_t corrupted;
    int ret;

    do {
        if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12) {
            status = MFXVideoCORE_SyncOperation(m_session->session,
                    m_frame->sync, VIRTIO_VIDEO_MSDK_TIME_TO_WAIT);
        } else {
            status = MFXVideoCORE_SyncOperation(m_session->session,
                    m_frame->vpp_sync, VIRTIO_VIDEO_MSDK_TIME_TO_WAIT);
        }
        if (status == MFX_WRN_IN_EXECUTION &&
                (stream->state == STREAM_STATE_CLEAR ||
                 stream->state == STREAM_STATE_TERMINATE)) {
            goto cancel;
        }
    } while (status == MFX_WRN_IN_EXECUTION);

    if (status != MFX_ERR_NONE) {
        error_report("virtio-video-decode/%d MFXVideoCORE_SyncOperation "
                      "failed: %d", stream->id, status);
        corrupted = 0;
    } else {
        corrupted = stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12 ?
              m_frame->surface->surface.Data.Corrupted :
              m_frame->vpp_surface->surface.Data.Corrupted;
    }

    if (status != MFX_ERR_NONE || corrupted != 0) {
        goto cancel;
    }

    if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12) {
        ret = virtio_video_msdk_output_surface(m_frame->surface, work_out->resource);
    } else {
        ret = virtio_video_msdk_output_surface(m_frame->vpp_surface, work_out->resource);
    }
    if (ret < 0)
        return -1;

    work_out->timestamp = work_in->timestamp;
    work_in->timestamp = 0;
    g_free(work_in->opaque);
    virtio_video_work_done(work_in);
    virtio_video_work_done(work_out);
    return 0;

cancel:
    /* Push output work back to output work queue */
    work_in->timestamp = 0;
    work_in->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
    g_free(work_in->opaque);
    virtio_video_work_done(work_in);
    qemu_mutex_lock(&stream->mutex);
    QTAILQ_INSERT_HEAD(&stream->output_work, work_out, next);
    qemu_mutex_unlock(&stream->mutex);
    return 0;
}

static void *virtio_video_decode_thread(void *arg)
{
    VirtIOVideoStream *stream = arg;
    VirtIOVideo *v = stream->parent;
    VirtIOVideoWork *work, *tmp_work;
    VirtIOVideoWork *work_out;
    VirtIOVideoCmd *cmd;
    MsdkSession *m_session = stream->opaque;
    int ret;

    while (true) {
        qemu_mutex_lock(&stream->mutex);
        switch (stream->state) {
        case STREAM_STATE_INIT:
            /* Waiting for the initial request which contains the header */
            if (QTAILQ_EMPTY(&stream->pending_work)) {
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_wait(&m_session->notifier);
                qemu_event_reset(&m_session->notifier);
                continue;
            }

            work = QTAILQ_FIRST(&stream->pending_work);
            if (virtio_video_decode_parse_header(work) < 0) {
                work->timestamp = 0;
                QTAILQ_REMOVE(&stream->pending_work, work, next);
                g_free(work->opaque);
                virtio_video_work_done(work);

                /*
                 * If the stream doesn't even begin to decode, then stream
                 * drain is meaningless.
                 */
                if (!QTAILQ_EMPTY(&stream->pending_work))
                    break;
                cmd = QTAILQ_FIRST(&stream->pending_cmds);
                if (cmd && cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                    QTAILQ_REMOVE(&stream->pending_cmds, cmd, next);
                    virtio_video_cmd_cancel(cmd);
                }
                break;
            }

            /* When the resolution event should be reported? */
            #if 1
            virtio_video_report_event(v,
                    VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED, stream->id);
            #endif

            /* initiate decoding for potentially multiple pending requests */
            QTAILQ_FOREACH_SAFE(work, &stream->pending_work, next, tmp_work) {
                ret = virtio_video_decode_submit_one_frame(work);
                if (ret < 0) {
                    if (ret != MFX_ERR_MORE_DATA) {
                        work->timestamp = 0;
                        work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                    }
                    QTAILQ_REMOVE(&stream->pending_work, work, next);
                    g_free(work->opaque);
                    virtio_video_work_done(work);
                }
            }

            cmd = QTAILQ_FIRST(&stream->pending_cmds);
            if (cmd && cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                stream->state = STREAM_STATE_DRAIN;
                break;
            }

            /* It is not allowed to change stream params from now on. */
            stream->state = STREAM_STATE_RUNNING;
            break;
        case STREAM_STATE_RUNNING:
            if (QTAILQ_EMPTY(&stream->input_work) || QTAILQ_EMPTY(&stream->output_work)) {
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_wait(&m_session->notifier);
                qemu_event_reset(&m_session->notifier);
                continue;
            }

            work = QTAILQ_FIRST(&stream->input_work);
            work_out = QTAILQ_FIRST(&stream->output_work);
            QTAILQ_REMOVE(&stream->input_work, work, next);
            QTAILQ_REMOVE(&stream->output_work, work_out, next);
            qemu_mutex_unlock(&stream->mutex);

            if (virtio_video_decode_retrieve_one_frame(work, work_out) < 0) {
                /* we can do nothing if guest buffer is too small */
                goto error;
            }
            continue;
        case STREAM_STATE_DRAIN:
            if (QTAILQ_EMPTY(&stream->output_work)) {
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_wait(&m_session->notifier);
                qemu_event_reset(&m_session->notifier);
                continue;
            }
            work_out = QTAILQ_FIRST(&stream->output_work);
            QTAILQ_REMOVE(&stream->output_work, work_out, next);

            /*
             * NOTE: According to frontend code, EOS buffer should be a standalone
             * empty buffer. This requirement will change when frontend driver
             * is updated.
             */
            if (QTAILQ_EMPTY(&stream->input_work)) {
                work_out->timestamp = 0;
                work_out->flags = VIRTIO_VIDEO_BUFFER_FLAG_EOS;
                virtio_video_work_done(work);

                cmd = QTAILQ_FIRST(&stream->pending_cmds);
                if (cmd == NULL ||
                        cmd->cmd_type != VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                    error_report("virtio-video: BUG: expected in-flight "
                                 "CMD_STREAM_DRAIN but not found");
                } else {
                    QTAILQ_REMOVE(&stream->pending_cmds, cmd, next);
                    virtio_video_cmd_done(cmd);
                }

                /*
                 * If guest starts decoding another bitstream, we can detect
                 * that through MFXVideoDECODE_DecodeFrameAsync return value
                 * and do reinitialization there.
                 */
                stream->state = STREAM_STATE_RUNNING;
                break;
            }

            work = QTAILQ_FIRST(&stream->input_work);
            QTAILQ_REMOVE(&stream->input_work, work, next);
            qemu_mutex_unlock(&stream->mutex);

            if (virtio_video_decode_retrieve_one_frame(work, work_out) < 0) {
                /* we can do nothing if guest buffer is too small */
                goto error;
            }
            continue;
        case STREAM_STATE_CLEAR:
            QTAILQ_FOREACH_SAFE(work, &stream->pending_work, next, tmp_work) {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                QTAILQ_REMOVE(&stream->pending_work, work, next);
                g_free(work->opaque);
                virtio_video_work_done(work);
            }
            QTAILQ_FOREACH_SAFE(work, &stream->input_work, next, tmp_work) {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                QTAILQ_REMOVE(&stream->input_work, work, next);
                g_free(work->opaque);
                virtio_video_work_done(work);
            }

            cmd = QTAILQ_FIRST(&stream->pending_cmds);
            if (cmd) {
                switch (cmd->cmd_type) {
                case VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL:
                    virtio_video_destroy_resource_list(stream, true);
                    /* fall through */
                case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
                    QTAILQ_REMOVE(&stream->pending_cmds, cmd, next);
                    virtio_video_cmd_done(cmd);
                    break;
                default:
                    error_report("virtio-video: BUG: expected in-flight "
                                 "CMD_RESOURCE_DESTROY_ALL/CMD_QUEUE_CLEAR "
                                 "but no found");
                    break;
                }
            } else {
                error_report("virtio-video: BUG: expected in-flight "
                             "CMD_RESOURCE_DESTROY_ALL/CMD_QUEUE_CLEAR "
                             "but no found");
            }

            if (QTAILQ_EMPTY(&stream->pending_cmds))
                stream->state = STREAM_STATE_RUNNING;
            break;
        case STREAM_STATE_TERMINATE:
            QTAILQ_FOREACH_SAFE(work, &stream->pending_work, next, tmp_work) {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                QTAILQ_REMOVE(&stream->pending_work, work, next);
                g_free(work->opaque);
                virtio_video_work_done(work);
            }
            QTAILQ_FOREACH_SAFE(work, &stream->input_work, next, tmp_work) {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                QTAILQ_REMOVE(&stream->input_work, work, next);
                g_free(work->opaque);
                virtio_video_work_done(work);
            }
            QTAILQ_FOREACH_SAFE(work, &stream->output_work, next, tmp_work) {
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                QTAILQ_REMOVE(&stream->output_work, work, next);
                virtio_video_work_done(work);
            }
            virtio_video_destroy_resource_list(stream, true);
            virtio_video_destroy_resource_list(stream, false);
            qemu_mutex_unlock(&stream->mutex);
            goto done;
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

    qemu_event_destroy(&m_session->notifier);
    virtio_video_msdk_uninit_surface_pools(m_session);
    return NULL;

error:
    virtqueue_detach_element(v->cmd_vq, work->elem, 0);
    virtqueue_detach_element(v->cmd_vq, work_out->elem, 0);
    g_free(work->opaque);
    g_free(work->elem);
    g_free(work_out->elem);
    g_free(work);
    g_free(work_out);
    virtio_video_report_event(v, VIRTIO_VIDEO_EVENT_ERROR, stream->id);
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
            error_report("CMD_STREAM_CREATE: stream %d already created",
                         resp->stream_id);
            return len;
        }
    }

    if (req->in_mem_type == VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT ||
            req->out_mem_type == VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT) {
        error_report("CMD_STREAM_CREATE: unsupported memory type (object)");
        return len;
    }

    QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next) {
        if (fmt->desc.format == req->coded_format) {
            break;
        }
    }
    if (fmt == NULL) {
        error_report("CMD_STREAM_CREATE: unsupported codec format %s",
                     virtio_video_format_name(req->coded_format));
        return len;
    }

    m_session = g_new0(MsdkSession, 1);
    status = MFXInitEx(param, &m_session->session);
    if (status != MFX_ERR_NONE) {
        error_report("CMD_STREAM_CREATE: MFXInitEx failed: %d", status);
        g_free(m_session);
        return len;
    }

    status = MFXVideoCORE_SetHandle(m_session->session, MFX_HANDLE_VA_DISPLAY,
                                    m_handle->va_handle);
    if (status != MFX_ERR_NONE) {
        error_report("CMD_STREAM_CREATE: MFXVideoCORE_SetHandle failed: %d",
                     status);
        MFXClose(m_session->session);
        g_free(m_session);
        return len;
    }

    stream = g_new0(VirtIOVideoStream, 1);
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
     * ARGB8 as the default format because virtio-gpu currently only support
     * this format. The frontend can change the format later, but if it uses
     * the default value, it can still work with virtio-gpu.
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
    #if 0
    stream->out.params.format = VIRTIO_VIDEO_FORMAT_ARGB8888;
    #else
    stream->out.params.format = VIRTIO_VIDEO_FORMAT_NV12;
    #endif
    printf("stream create, use %s as output format\n", virtio_video_fmt_to_string(stream->out.params.format));
    stream->out.params.min_buffers = 1;
    stream->out.params.max_buffers = 32;
    stream->out.params.frame_rate = fmt->frames.lh_first->frame_rates[0].max;
    stream->out.params.frame_width = fmt->frames.lh_first->frame.width.max;
    stream->out.params.frame_height = fmt->frames.lh_first->frame.height.max;
    stream->out.params.crop.left = 0;
    stream->out.params.crop.top = 0;
    stream->out.params.crop.width = stream->out.params.frame_width;
    stream->out.params.crop.height = stream->out.params.frame_height;
    stream->out.params.num_planes = 1;
    stream->out.params.plane_formats[0].plane_size =
        stream->out.params.frame_width * stream->out.params.frame_height * 4;
    stream->out.params.plane_formats[0].stride =
        stream->out.params.frame_width * 4;

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
    for (i = 0; i < VIRTIO_VIDEO_QUEUE_NUM; i++) {
        QLIST_INIT(&stream->resource_list[i]);
    }
    QTAILQ_INIT(&stream->pending_cmds);
    QTAILQ_INIT(&stream->pending_work);
    QTAILQ_INIT(&stream->input_work);
    QTAILQ_INIT(&stream->output_work);
    qemu_mutex_init(&stream->mutex);

    qemu_event_init(&m_session->notifier, false);
    QLIST_INIT(&m_session->surface_pool);
    QLIST_INIT(&m_session->vpp_surface_pool);

    snprintf(thread_name, sizeof(thread_name), "virtio-video-decode/%d",
             stream->id);
    qemu_thread_create(&m_session->thread, thread_name,
                       virtio_video_decode_thread, stream,
                       QEMU_THREAD_JOINABLE);

    QLIST_INSERT_HEAD(&v->stream_list, stream, next);
    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;

    DPRINTF("CMD_STREAM_CREATE: stream %d [%s] created\n",
            stream->id, stream->tag);
    return len;
}

static int virtio_video_msdk_dec_stream_terminate(VirtIOVideoStream *stream)
{
    VirtIOVideoCmd *cmd, *tmp_cmd;
    MsdkSession *m_session = stream->opaque;

    qemu_mutex_lock(&stream->mutex);
    switch (stream->state) {
    case STREAM_STATE_INIT:
        cmd = QTAILQ_FIRST(&stream->pending_cmds);
        if (cmd && cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
            QTAILQ_REMOVE(&stream->pending_cmds, cmd, next);
            virtio_video_cmd_cancel(cmd);
        }
        break;
    case STREAM_STATE_RUNNING:
        break;
    case STREAM_STATE_DRAIN:
        cmd = QTAILQ_FIRST(&stream->pending_cmds);
        if (cmd == NULL || cmd->cmd_type != VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
            error_report("virtio-video: BUG: expected in-flight "
                         "CMD_STREAM_DRAIN but not found");
        } else {
            QTAILQ_REMOVE(&stream->pending_cmds, cmd, next);
            virtio_video_cmd_cancel(cmd);
        }
        break;
    case STREAM_STATE_CLEAR:
        QTAILQ_FOREACH_SAFE(cmd, &stream->pending_cmds, next, tmp_cmd) {
            if (cmd->cmd_type != VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL &&
                cmd->cmd_type != VIRTIO_VIDEO_CMD_QUEUE_CLEAR) {
                break;
            }
            QTAILQ_REMOVE(&stream->pending_cmds, cmd, next);
            virtio_video_cmd_cancel(cmd);
        }
        break;
    case STREAM_STATE_TERMINATE:
        DPRINTF("stream %d already terminated\n", stream->id);
        qemu_mutex_unlock(&stream->mutex);
        return -1;
    default:
        break;
    }

    /* There should be no pending commands according our protocol. */
    if (!QTAILQ_EMPTY(&stream->pending_cmds)) {
        error_report("virtio-video: BUG: found in-flight commands "
                     "while expected none");
        QTAILQ_FOREACH_SAFE(cmd, &stream->pending_cmds, next, tmp_cmd) {
            QTAILQ_REMOVE(&stream->pending_cmds, cmd, next);
            virtio_video_cmd_cancel(cmd);
        }
    }

    stream->state = STREAM_STATE_TERMINATE;
    qemu_event_set(&m_session->notifier);
    qemu_mutex_unlock(&stream->mutex);
    return 0;
}

size_t virtio_video_msdk_dec_stream_destroy(VirtIOVideo *v,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideoStream *stream;
    MsdkSession *m_session;
    size_t len;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            m_session = stream->opaque;
            break;
        }
    }
    if (stream == NULL) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        return len;
    }

    if (virtio_video_msdk_dec_stream_terminate(stream) < 0)
        return len;

    qemu_thread_join(&m_session->thread);
    QLIST_REMOVE(stream, next);
    qemu_mutex_destroy(&stream->mutex);
    g_free(m_session);
    if (stream->bitstream_header.Data) {
        g_free(stream->bitstream_header.Data);
        stream->bitstream_header.Data = NULL;
    }
    g_free(stream);

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    return len;
}

/**
 * Protocol:
 *
 * @STREAM_STATE_INIT:  There can be one and only one CMD_STREAM_DRAIN pending
 *                      in @pending_cmds, waiting for the initialization of
 *                      decode thread. The stream will then enter
 *                      STREAM_STATE_DRAIN after initialization is done.
 * 
 * @STREAM_STATE_DRAIN: There is one and only one in-flight CMD_STREAM_DRAIN in
 *                      @pending_cmds. CMD_STREAM_DRAIN must fail in this
 *                      state.
 */
size_t virtio_video_msdk_dec_stream_drain(VirtIOVideo *v,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideoStream *stream;
    VirtIOVideoCmd *cmd;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("CMD_STREAM_DRAIN: stream %d not found",
                     req->hdr.stream_id);
        return len;
    }

    qemu_mutex_lock(&stream->mutex);
    switch (stream->state) {
    case STREAM_STATE_INIT:
        cmd = QTAILQ_FIRST(&stream->pending_cmds);
        if (cmd && cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN)
            break;
        if (QTAILQ_EMPTY(&stream->pending_work))
            break;
        /* fall through */
    case STREAM_STATE_RUNNING:
        cmd = g_new(VirtIOVideoCmd, 1);
        cmd->parent = stream;
        cmd->elem = elem;
        cmd->cmd_type = VIRTIO_VIDEO_CMD_STREAM_DRAIN;
        QTAILQ_INSERT_TAIL(&stream->pending_cmds, cmd, next);
        if (stream->state == STREAM_STATE_RUNNING)
            stream->state = STREAM_STATE_DRAIN;
        DPRINTF("CMD_STREAM_CREATE (async): stream %d start to drain\n",
                stream->id);
        qemu_mutex_unlock(&stream->mutex);
        return 0;
    case STREAM_STATE_DRAIN:
    case STREAM_STATE_CLEAR:
    case STREAM_STATE_TERMINATE:
        /* Return VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION */
        break;
    default:
        break;
    }
    DPRINTF("CMD_STREAM_DRAIN: stream %d currently unable to "
            "serve the request", stream->id);
    qemu_mutex_unlock(&stream->mutex);
    return len;
}

static int work_id=0;
size_t virtio_video_msdk_dec_resource_queue(VirtIOVideo *v,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp,
    VirtQueueElement *elem)
{
    MsdkSession *m_session;
    MsdkFrame *m_frame;
    VirtIOVideoStream *stream;
    VirtIOVideoCmd *cmd;
    VirtIOVideoResource *resource;
    VirtIOVideoWork *work;
    size_t len;
    int i;

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
        error_report("CMD_RESOURCE_QUEUE: stream %d not found",
                     req->hdr.stream_id);
        return len;
    }

    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        QLIST_FOREACH(resource,
                &stream->resource_list[VIRTIO_VIDEO_QUEUE_INPUT], next) {
            if (resource->id == req->resource_id) {
                break;
            }
        }
        if (resource == NULL) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
            error_report("CMD_RESOURCE_QUEUE: stream %d input resource %d "
                         "not found", stream->id, req->resource_id);
            break;
        }

        if (!virtio_video_format_is_valid(stream->in.params.format,
                                          req->num_data_sizes)) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
            error_report("CMD_RESOURCE_QUEUE: stream %d try to queue "
                         "a resource with num_data_sizes=%d for input queue "
                         "whose format is %s", stream->id, req->num_data_sizes,
                         virtio_video_format_name(stream->in.params.format));
            break;
        }

        qemu_mutex_lock(&stream->mutex);
        QTAILQ_FOREACH(work, &stream->pending_work, next) {
            if (resource->id == work->resource->id) {
                goto error;
            }
        }
        QTAILQ_FOREACH(work, &stream->input_work, next) {
            if (resource->id == work->resource->id) {
            error:
                error_report("CMD_RESOURCE_QUEUE: stream %d input resource %d "
                             "already queued, cannot be queued again",
                             stream->id, resource->id);
                qemu_mutex_unlock(&stream->mutex);
                return len;
            }
        }

        m_frame = g_new0(MsdkFrame, 1);
        m_frame->bitstream.Data = resource->slices[0]->page.hva;
        m_frame->bitstream.DataLength = req->data_sizes[0];
        m_frame->bitstream.MaxLength = req->data_sizes[0];
        printf("VIRTIO_VIDEO_QUEUE_TYPE_INPUT, data:%p, len:%d, maxLen:%d\n", m_frame->bitstream.Data, m_frame->bitstream.DataLength, m_frame->bitstream.MaxLength);

        work = g_new0(VirtIOVideoWork, 1);
        work->parent = stream;
        work->elem = elem;
        work->resource = resource;
        work->queue_type = req->queue_type;
        work->timestamp = req->timestamp;
        work->opaque = m_frame;
        work->id = work_id++;
        printf("dyang23, create work id:%d\n", work->id);

        switch (stream->state) {
        case STREAM_STATE_INIT:
            /* There can be one CMD_STREAM_DRAIN pending while in STREAM_STATE_INIT. */
            cmd = QTAILQ_FIRST(&stream->pending_cmds);
            if (cmd && cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                g_free(work->opaque);
                g_free(work);
                qemu_mutex_unlock(&stream->mutex);
                DPRINTF("CMD_RESOURCE_QUEUE: stream %d currently unable to "
                        "queue input resources\n", stream->id);
                return len;
            }

            /* Do not decode the frame in case the initialization is not done yet. */
            qemu_event_set(&m_session->notifier);
            break;
        case STREAM_STATE_RUNNING:
            aio_bh_schedule_oneshot(v->ctx,
                    virtio_video_decode_submit_one_frame_bh, work);
            break;
        case STREAM_STATE_DRAIN:
        case STREAM_STATE_CLEAR:
        case STREAM_STATE_TERMINATE:
            /* Return VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION */
            g_free(work->opaque);
            g_free(work);
            qemu_mutex_unlock(&stream->mutex);
            DPRINTF("CMD_RESOURCE_QUEUE: stream %d currently unable to "
                    "queue input resources\n", stream->id);
            return len;
        default:
            break;
        }

        /*
         * Input resources first go to the pending work queue. After the
         * DecodeFrameAsync function is called for them, they are moved to the
         * input work queue.
         */
        QTAILQ_INSERT_TAIL(&stream->pending_work, work, next);
        qemu_mutex_unlock(&stream->mutex);

        DPRINTF("CMD_RESOURCE_QUEUE: stream %d queued input resource %d\n",
                stream->id, resource->id);
        len = 0;
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        QLIST_FOREACH(resource,
                &stream->resource_list[VIRTIO_VIDEO_QUEUE_OUTPUT], next) {
            if (resource->id == req->resource_id) {
                break;
            }
        }
        if (resource == NULL) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
            error_report("CMD_RESOURCE_QUEUE: stream %d output resource %d "
                         "not found", stream->id, req->resource_id);
            break;
        }

        if (!virtio_video_format_is_valid(stream->out.params.format,
                                          req->num_data_sizes)) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
            error_report("CMD_RESOURCE_QUEUE: stream %d try to queue "
                         "a resource with num_data_sizes=%d for output queue "
                         "whose format is %s", stream->id, req->num_data_sizes,
                         virtio_video_format_name(stream->out.params.format));
            break;
        }

        /* The output size is determined by backend. */
        for (i = 0; i < req->num_data_sizes; i++) {
            if (req->data_sizes[i] != 0) {
                error_report("CMD_RESOURCE_QUEUE: stream %d try to queue "
                             "a resource with data_sizes[%d]=%d for "
                             "output queue, which is not allowed",
                             stream->id, i, req->data_sizes[i]);
                resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
                break;
            }
        }

        /* Only input resources have timestamp assigned. */
        if (req->timestamp != 0) {
            error_report("CMD_RESOURCE_QUEUE: stream %d try to assign "
                         "timestamp 0x%llx to output resource, which is "
                         "not allowed", stream->id, req->timestamp);
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
            break;
        }

        qemu_mutex_lock(&stream->mutex);
        QTAILQ_FOREACH(work, &stream->output_work, next) {
            if (resource->id == work->resource->id) {
                error_report("CMD_RESOURCE_QUEUE: stream %d output resource %d "
                             "already queued, cannot be queued again",
                             stream->id, resource->id);
                qemu_mutex_unlock(&stream->mutex);
                return len;
            }
        }
        if (stream->state == STREAM_STATE_TERMINATE) {
            qemu_mutex_unlock(&stream->mutex);
            DPRINTF("CMD_RESOURCE_QUEUE: stream %d currently unable to "
                    "queue output resources\n", stream->id);
            return len;
        }

        work = g_new0(VirtIOVideoWork, 1);
        work->parent = stream;
        work->elem = elem;
        work->resource = resource;
        work->queue_type = req->queue_type;
        work->id = work_id++;
        printf("dyang23, create work output id:%d\n", work->id);

        /*
         * Output resources are just containers for decoded frames. They go
         * directly to output work queue since no MediaSDK task is required to
         * be submitted.
         */
        QTAILQ_INSERT_TAIL(&stream->output_work, work, next);
        qemu_event_set(&m_session->notifier);
        qemu_mutex_unlock(&stream->mutex);

        DPRINTF("CMD_RESOURCE_QUEUE: stream %d queued output resource %d\n",
                stream->id, resource->id);
        len = 0;
        break;
    default:
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_RESOURCE_QUEUE: invalid queue type 0x%x",
                     req->queue_type);
        break;
    }

    return len;
}

/**
 * Protocol:
 *
 * @STREAM_STATE_CLEAR: In this state, there can be one and only one
 *                      CMD_QUEUE_CLEAR pending in @pending_cmds. There can be
 *                      one or more CMD_RESOURCE_DESTROY_ALL pending in
 *                      @pending_cmds since the operation is idempotent and
 *                      issuing the command while the last one is being
 *                      processed is not prohibited by the spec.
 */
static size_t virtio_video_msdk_dec_resource_clear(VirtIOVideoStream *stream,
    uint32_t queue_type, virtio_video_cmd_hdr *resp, VirtQueueElement *elem,
    bool destroy)
{
    VirtIOVideoCmd *cmd;
    VirtIOVideoWork *work, *tmp_work;
    MsdkSession *m_session = stream->opaque;

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    switch (queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        qemu_mutex_lock(&stream->mutex);
        cmd = QTAILQ_FIRST(&stream->pending_cmds);

        switch (stream->state) {
        case STREAM_STATE_INIT:
            /* Cancel the pending CMD_STREAM_DRAIN */
            if (cmd && cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                QTAILQ_REMOVE(&stream->pending_cmds, cmd, next);
                virtio_video_cmd_cancel(cmd);
            }

            QTAILQ_FOREACH_SAFE(work, &stream->pending_work, next, tmp_work) {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                QTAILQ_REMOVE(&stream->pending_work, work, next);
                g_free(work->opaque);
                virtio_video_work_done(work);
            }
            if (destroy) {
                virtio_video_destroy_resource_list(stream, true);
                DPRINTF("CMD_RESOURCE_DESTROY_ALL: stream %d input resources "
                        "destroyed\n", stream->id);
            } else {
                DPRINTF("CMD_QUEUE_CLEAR: stream %d input queue cleared\n",
                        stream->id);
            }
            break;
        case STREAM_STATE_RUNNING:
            stream->state = STREAM_STATE_CLEAR;
enqueue:
            cmd = g_new(VirtIOVideoCmd, 1);
            cmd->parent = stream;
            cmd->elem = elem;
            cmd->cmd_type = destroy ? VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL :
                                      VIRTIO_VIDEO_CMD_QUEUE_CLEAR;
            QTAILQ_INSERT_TAIL(&stream->pending_cmds, cmd, next);
            qemu_event_set(&m_session->notifier);

            if (destroy) {
                DPRINTF("CMD_RESOURCE_DESTROY_ALL (async): stream %d start to "
                        "destroy input resources\n", stream->id);
            } else {
                DPRINTF("CMD_QUEUE_CLEAR (async): stream %d start to "
                        "clear input queue\n", stream->id);
            }
            qemu_mutex_unlock(&stream->mutex);
            return 0;
        case STREAM_STATE_DRAIN:
            if (cmd == NULL || cmd->cmd_type != VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                error_report("virtio-video: BUG: expected in-flight "
                             "CMD_STREAM_DRAIN but not found");
            } else {
                QTAILQ_REMOVE(&stream->pending_cmds, cmd, next);
                virtio_video_cmd_cancel(cmd);
            }
            stream->state = STREAM_STATE_CLEAR;
            goto enqueue;
        case STREAM_STATE_CLEAR:
            /* CMD_RESOURCE_DESTROY_ALL is idempotent */
            if (destroy)
                goto enqueue;

            /* CMD_QUEUE_CLEAR should fail if there is already one in-flight */
            QTAILQ_FOREACH(cmd, &stream->pending_cmds, next) {
                if (cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) {
                    break;
                }
            }
            if (cmd == NULL)
                goto enqueue;

            /* fall through */
        case STREAM_STATE_TERMINATE:
            DPRINTF("%s: stream %d currently unable to serve the request",
                    destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                    stream->id);
            /* fall through */
        default:
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            break;
        }
        qemu_mutex_unlock(&stream->mutex);
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        qemu_mutex_lock(&stream->mutex);
        if (stream->state == STREAM_STATE_TERMINATE) {
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            DPRINTF("%s: stream %d currently unable to serve the request",
                    destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                    stream->id);
            qemu_mutex_unlock(&stream->mutex);
            break;
        }

        QTAILQ_FOREACH_SAFE(work, &stream->output_work, next, tmp_work) {
            work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
            QTAILQ_REMOVE(&stream->output_work, work, next);
            virtio_video_work_done(work);
        }
        if (destroy) {
            virtio_video_destroy_resource_list(stream, false);
            DPRINTF("CMD_RESOURCE_DESTROY_ALL: stream %d output resources "
                    "destroyed\n", stream->id);
        } else {
            DPRINTF("CMD_QUEUE_CLEAR: stream %d output queue cleared\n",
                    stream->id);
        }
        qemu_mutex_unlock(&stream->mutex);
        break;
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("%s: invalid queue type 0x%x",
                     destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                     queue_type);
        break;
    }

    return sizeof(*resp);
}

size_t virtio_video_msdk_dec_resource_destroy_all(VirtIOVideo *v,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideoStream *stream;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        error_report("CMD_RESOURCE_DESTROY_ALL: stream %d not found",
                     req->hdr.stream_id);
        return sizeof(*resp);
    }

    return virtio_video_msdk_dec_resource_clear(stream, req->queue_type,
                                                resp, elem, true);
}

size_t virtio_video_msdk_dec_queue_clear(VirtIOVideo *v,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
    VirtIOVideoStream *stream;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        error_report("CMD_QUEUE_CLEAR: stream %d not found",
                     req->hdr.stream_id);
        return sizeof(*resp);
    }

    return virtio_video_msdk_dec_resource_clear(stream, req->queue_type,
                                                resp, elem, false);
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
    if (stream == NULL) {
        error_report("CMD_GET_PARAMS: stream %d not found",
                     req->hdr.stream_id);
        return len;
    }

    resp->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_PARAMS;
    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        memcpy(&resp->params, &stream->in.params, sizeof(resp->params));
        DPRINTF("CMD_GET_PARAMS: reported input params\n");
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        memcpy(&resp->params, &stream->out.params, sizeof(resp->params));
        DPRINTF("CMD_GET_PARAMS: reported output params\n");
        break;
    default:
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_GET_PARAMS: invalid queue type 0x%x", req->queue_type);
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
    if (stream == NULL) {
        error_report("CMD_SET_PARAMS: stream %d not found",
                     req->hdr.stream_id);
        return len;
    }

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    qemu_mutex_lock(&stream->mutex);
    switch (req->params.queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        if (stream->state != STREAM_STATE_INIT) {
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            error_report("CMD_SET_PARAMS: stream %d is not allowed to change "
                         "param after decoding has started", stream->id);
            break;
        }

        if (!virtio_video_format_is_codec(req->params.format)) {
            error_report("CMD_SET_PARAMS: stream %d try to set decoder "
                         "input queue format to %s", stream->id,
                         virtio_video_format_name(req->params.format));
            break;
        }

        stream->in.params.format = req->params.format;
        stream->control.profile = 0;
        stream->control.level = 0;
        switch (req->params.format) {
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
        stream->in.params.num_planes = req->params.num_planes;
        stream->in.params.plane_formats[0] = req->params.plane_formats[0];

        if (virtio_video_param_fixup(&stream->in.params)) {
            DPRINTF("CMD_SET_PARAMS: incompatible parameters, "
                    "fixed up automatically\n");
        }
        DPRINTF("CMD_SET_PARAMS: stream %d set input format to %s\n",
                stream->id, virtio_video_format_name(req->params.format));
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        if (stream->state != STREAM_STATE_INIT) {
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            error_report("CMD_SET_PARAMS: stream %d is not allowed to change "
                         "param after decoding has started", stream->id);
        }

        if (virtio_video_format_is_codec(req->params.format)) {
            error_report("CMD_SET_PARAMS: stream %d try to set decoder "
                         "output queue format to %s", stream->id,
                         virtio_video_format_name(req->params.format));
            break;
        }

        /*
         * Output parameters should be derived from input bitstream. The
         * frontend is only allowed to change pixel format.
         *
         * TODO: figure out if we should also allow setting output crop
         */
        printf("set output queue: format:%d, %s\n", req->params.format, virtio_video_fmt_to_string(req->params.format));
        stream->out.params.format = req->params.format;
        printf("set output queue: num_planes:%d\n", req->params.num_planes);
        stream->out.params.num_planes = req->params.num_planes;
        for (i = 0; i < req->params.num_planes; i++) {
            stream->out.params.plane_formats[i] = req->params.plane_formats[i];
        }

        if (virtio_video_param_fixup(&stream->out.params)) {
            DPRINTF("CMD_SET_PARAMS: incompatible parameters, "
                    "fixed up automatically\n");
        }
        DPRINTF("CMD_SET_PARAMS: stream %d set output format to %s\n",
                stream->id, virtio_video_format_name(req->params.format));
        break;
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_SET_PARAMS: invalid queue type 0x%x",
                     req->params.queue_type);
        break;
    }
    qemu_mutex_unlock(&stream->mutex);
    return len;
}

size_t virtio_video_msdk_dec_query_control(VirtIOVideo *v,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp)
{
    VirtIOVideoFormat *fmt;
    void *req_buf = (char *)req + sizeof(*req);
    void *resp_buf;
    size_t len = sizeof(**resp);

    switch (req->control) {
    case VIRTIO_VIDEO_CONTROL_PROFILE:
    {
        virtio_video_query_control_profile *query = req_buf;
        virtio_video_query_control_resp_profile *resp_profile;

        QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next) {
            if (fmt->desc.format == query->format) {
                break;
            }
        }
        if (fmt == NULL) {
            error_report("CMD_QUERY_CONTROL: format %s not supported",
                         virtio_video_format_name(query->format));
            goto error;
        }
        if (fmt->profile.num == 0) {
            error_report("CMD_QUERY_CONTROL: format %s does not support "
                         "profiles", virtio_video_format_name(query->format));
            goto error;
        }

        len += sizeof(*resp_profile) + sizeof(uint32_t) * fmt->profile.num;
        *resp = g_malloc0(len);

        resp_profile = resp_buf = (char *)(*resp) + sizeof(**resp);
        resp_profile->num = fmt->profile.num;
        resp_buf += sizeof(*resp_profile);
        memcpy(resp_buf, fmt->profile.values,
               sizeof(uint32_t) * fmt->profile.num);

        DPRINTF("CMD_QUERY_CONTROL: format %s reported %d supported profiles\n",
                virtio_video_format_name(query->format), fmt->profile.num);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_LEVEL:
    {
        virtio_video_query_control_level *query = req_buf;
        virtio_video_query_control_resp_level *resp_level;

        QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next) {
            if (fmt->desc.format == query->format) {
                break;
            }
        }
        if (fmt == NULL) {
            error_report("CMD_QUERY_CONTROL: format %s not supported",
                         virtio_video_format_name(query->format));
            goto error;
        }
        if (fmt->level.num == 0) {
            error_report("CMD_QUERY_CONTROL: format %s does not support "
                         "levels", virtio_video_format_name(query->format));
            goto error;
        }

        len += sizeof(*resp_level) + sizeof(uint32_t) * fmt->level.num;
        *resp = g_malloc0(len);

        resp_level = resp_buf = (char *)(*resp) + sizeof(**resp);
        resp_level->num = fmt->level.num;
        resp_buf += sizeof(*resp_level);
        memcpy(resp_buf, fmt->level.values, sizeof(uint32_t) * fmt->level.num);

        DPRINTF("CMD_QUERY_CONTROL: format %s reported %d supported levels\n",
                virtio_video_format_name(query->format), fmt->level.num);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_BITRATE:
        error_report("CMD_QUERY_CONTROL: virtio-video-dec does not support "
                     "bitrate");
        goto error;
    default:
        error_report("CMD_QUERY_CONTROL: unsupported control type 0x%x",
                     req->control);
        goto error;
    }

    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL;
    (*resp)->hdr.stream_id = req->hdr.stream_id;
    return len;

error:
    *resp = g_malloc(sizeof(**resp));
    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
    (*resp)->hdr.stream_id = req->hdr.stream_id;
    return sizeof(**resp);
}

size_t virtio_video_msdk_dec_get_control(VirtIOVideo *v,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp)
{
    VirtIOVideoStream *stream;
    size_t len = sizeof(**resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        *resp = g_malloc(sizeof(**resp));
        (*resp)->hdr.stream_id = req->hdr.stream_id;
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("CMD_GET_CONTROL: stream %d not found",
                     req->hdr.stream_id);
        return len;
    }

    switch (req->control) {
    case VIRTIO_VIDEO_CONTROL_BITRATE:
    {
        virtio_video_control_val_bitrate *val;

        if (stream->control.bitrate == 0)
            goto error;

        len += sizeof(*val);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(**resp);
        val->bitrate = stream->control.bitrate;
        DPRINTF("CMD_GET_CONTROL: stream %d reports bitrate = %d\n",
                stream->id, val->bitrate);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_PROFILE:
    {
        virtio_video_control_val_profile *val;

        if (stream->control.profile == 0)
            goto error;

        len += sizeof(*val);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(**resp);
        val->profile = stream->control.profile;
        DPRINTF("CMD_GET_CONTROL: stream %d reports profile = %d\n",
                stream->id, val->profile);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_LEVEL:
    {
        virtio_video_control_val_level *val;

        if (stream->control.level == 0)
            goto error;

        len += sizeof(*val);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(**resp);
        val->level = stream->control.level;
        DPRINTF("CMD_GET_CONTROL: stream %d reports level = %d\n",
                stream->id, val->level);
        break;
    }
    default:
error:
        *resp = g_malloc(sizeof(**resp));
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
        error_report("CMD_GET_CONTROL: stream %d does not support "
                     "control type 0x%x", stream->id, req->control);
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
    error_report("CMD_SET_CONTROL: not allowed in virtio-video-dec");
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
        return -1;
    }

    status = MFXInitEx(init_param, &session);
    if (status != MFX_ERR_NONE) {
        error_report("MFXInitEx failed: %d", status);
        return -1;
    }

    m_handle = v->opaque;
    status = MFXVideoCORE_SetHandle(session, MFX_HANDLE_VA_DISPLAY, m_handle->va_handle);
    if (status != MFX_ERR_NONE) {
        error_report("MFXVideoCORE_SetHandle failed: %d", status);
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
            DPRINTF("Input codec %s isn't supported by MediaSDK: %d\n",
                    virtio_video_format_name(in_format), status);
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
            DPRINTF("Failed to query frame size for input codec %s\n",
                    virtio_video_format_name(in_format));
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
        QLIST_INSERT_HEAD(&v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], in_fmt, next);

        DPRINTF("Input capability %s: "
                "width [%d, %d] @%d, height [%d, %d] @%d, rate [%d, %d] @%d\n",
                virtio_video_format_name(in_format),
                w_min, w_max, in_fmt_frame->frame.width.step,
                h_min, h_max, in_fmt_frame->frame.height.step,
                in_fmt_frame->frame_rates[0].min,
                in_fmt_frame->frame_rates[0].max,
                in_fmt_frame->frame_rates[0].step);

        /* Query supported profiles */
        if (virtio_video_format_profile_range(in_format, &ctrl_min, &ctrl_max) < 0)
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
        if (virtio_video_format_level_range(in_format, &ctrl_min, &ctrl_max) < 0)
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
    in_fmt = QLIST_FIRST(&v->format_list[VIRTIO_VIDEO_QUEUE_INPUT]);
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
        QLIST_INSERT_HEAD(&v->format_list[VIRTIO_VIDEO_QUEUE_OUTPUT], out_fmt, next);

        DPRINTF("Output capability %s: "
                "width [%d, %d] @%d, height [%d, %d] @%d, rate [%d, %d] @%d\n",
                virtio_video_format_name(out_format[i]),
                out_fmt_frame->frame.width.min, out_fmt_frame->frame.width.max,
                out_fmt_frame->frame.width.step, out_fmt_frame->frame.height.min,
                out_fmt_frame->frame.height.max, out_fmt_frame->frame.height.step,
                out_fmt_frame->frame_rates[0].min, out_fmt_frame->frame_rates[0].max,
                out_fmt_frame->frame_rates[0].step);
    }

    QLIST_FOREACH(in_fmt, &v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next) {
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
    MsdkSession *m_session;

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, tmp_stream) {
        m_session = stream->opaque;

        virtio_video_msdk_dec_stream_terminate(stream);
        qemu_thread_join(&m_session->thread);

        QLIST_REMOVE(stream, next);
        qemu_mutex_destroy(&stream->mutex);
        g_free(m_session);
        if (stream->bitstream_header.Data) {
            g_free(stream->bitstream_header.Data);
            stream->bitstream_header.Data = NULL;
        }
        g_free(stream);
    }

    virtio_video_msdk_uninit_handle(v);
}
