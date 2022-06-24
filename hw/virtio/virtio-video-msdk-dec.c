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
#include "virtio-video-va-allocator.h"
#include "mfx/mfxvideo.h"

#define THREAD_NAME_LEN 48

#define VIRTIO_VIDEO_DRM_DEVICE "/dev/dri/by-path/pci-0000:00:02.0-render"

//#define VIRTIO_VIDEO_MSDK_DEC_DEBUG 1
//#define DUMP_SURFACE
//#define DUMP_SURFACE_END
//#define DUMP_SURFACE_BEFORE
#if !defined VIRTIO_VIDEO_MSDK_DEC_DEBUG && !defined DEBUG_VIRTIO_VIDEO_ALL
#undef DPRINTF
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define OUTPUT_RGBA

static void virtio_video_msdk_bitstream_append(mfxBitstream *this,
    mfxBitstream *other) {
    if (other->DataLength == 0)
        return;

    memmove(this->Data, this->Data + this->DataOffset, this->DataLength);
    this->DataOffset = 0;
    if (this->MaxLength < this->DataLength + other->DataLength) {
        this->MaxLength = this->DataLength + other->DataLength;
        this->Data = g_realloc(this->Data, this->MaxLength);
    }
    MEMCPY_S(this->Data + this->DataLength, other->Data + other->DataOffset,
           other->DataLength, other->DataLength);
    this->DataLength += other->DataLength;
}

static mfxStatus virtio_video_decode_parse_header(VirtIOVideoWork *work)
{
    VirtIOVideoStream *stream = work->parent;
    MsdkSession *m_session = stream->opaque;
    mfxStatus status;
    mfxVideoParam param = {0}, vpp_param = {0};
    mfxFrameAllocRequest alloc_req, vpp_req[2];
    mfxBitstream *input = work->opaque;
    mfxBitstream *bitstream = &m_session->bitstream;

    memset(&alloc_req, 0, sizeof(alloc_req));
    memset(&vpp_req, 0, sizeof(alloc_req) * 2);

    if (work->queue_type != VIRTIO_VIDEO_QUEUE_TYPE_INPUT)
        return MFX_ERR_UNDEFINED_BEHAVIOR;

    virtio_video_msdk_load_plugin(m_session->session, stream->in.params.format,
                                  false);
    if (virtio_video_msdk_init_param_dec(m_session, &param, stream) < 0)
        return MFX_ERR_UNSUPPORTED;

    virtio_video_msdk_bitstream_append(bitstream, input);
    status = MFXVideoDECODE_DecodeHeader(m_session->session, bitstream, &param);

    switch (status) {
    case MFX_ERR_NONE:
        break;
    case MFX_ERR_MORE_DATA:
        DPRINTF("virtio-video-decode/%d MFXVideoDECODE_DecodeHeader failed, "
                "waiting for more input buffers\n", stream->id);
        return status;
    default:
        error_report("virtio-video-decode/%d MFXVideoDECODE_DecodeHeader "
                     "failed: %d", stream->id, status);
        return status;
    }

    status = MFXVideoDECODE_QueryIOSurf(m_session->session, &param, &alloc_req);
    if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
        error_report("virtio-video-decode/%d MFXVideoDECODE_QueryIOSurf "
                      "failed: %d", stream->id, status);
        return status;
    }

    if (0 == param.mfx.FrameInfo.FrameRateExtN ||
        0 == param.mfx.FrameInfo.FrameRateExtD) {
        DPRINTF("No FrameRate in header, using default FrameRate 30\n");
        param.mfx.FrameInfo.FrameRateExtN = 30;
        param.mfx.FrameInfo.FrameRateExtD = 1;
    }

    status = MFXVideoDECODE_Init(m_session->session, &param);
    if (status != MFX_ERR_NONE && status != MFX_WRN_PARTIAL_ACCELERATION) {
        error_report("virtio-video-decode/%d MFXVideoDECODE_Init "
                     "failed: %d",
                     stream->id, status);
        return status;
    }

    param.AsyncDepth = 1;
    printf_mfxVideoParam(&param);

    if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12)
        goto done;

    if (virtio_video_msdk_init_vpp_param_dec(&param, &vpp_param, stream) < 0) {
        MFXVideoDECODE_Close(m_session->session);
        return MFX_ERR_UNSUPPORTED;
    }

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
    int vpp_pool_size = (vpp_req[1].NumFrameSuggested + alloc_req.NumFrameSuggested) * 2;
    if (vpp_pool_size > 30)
        m_session->vpp_surface_num = 30;
    else if (vpp_pool_size < 16)
        m_session->vpp_surface_num = 16;
    else
        m_session->vpp_surface_num = vpp_pool_size;
    if (vpp_param.IOPattern & MFX_IOPATTERN_OUT_VIDEO_MEMORY)
        virtio_video_msdk_init_vpp_video_surface_pool(m_session, &vpp_req[1],
                                                      &vpp_param.vpp.Out);
    else
        virtio_video_msdk_init_surface_pool(m_session, &vpp_req[1],
                                            &vpp_param.vpp.Out, true, false);
done:
    m_session->surface_num += alloc_req.NumFrameSuggested;
    if (param.IOPattern != MFX_IOPATTERN_OUT_VIDEO_MEMORY)
        virtio_video_msdk_init_surface_pool(m_session, &alloc_req,
                                            &param.mfx.FrameInfo, false, false);
    stream->out.params.min_buffers = alloc_req.NumFrameMin;
    stream->out.params.max_buffers = alloc_req.NumFrameSuggested;
    virtio_video_msdk_stream_reset_param(stream, &param, false);
    return MFX_ERR_NONE;

error_vpp:
    MFXVideoDECODE_Close(m_session->session);
    return status;
}

static mfxStatus virtio_video_decode_one_frame(VirtIOVideoWork *work,
    MsdkFrame *m_frame, bool eos)
{
    VirtIOVideoStream *stream = work->parent;
    MsdkSession *m_session = stream->opaque;
    MsdkSurface *work_surface = NULL, *vpp_work_surface = NULL;
    mfxStatus status;
    mfxVideoParam param = {0};
    mfxBitstream *bitstream = &m_session->bitstream;
    mfxFrameSurface1 *out_surface = NULL;

    bitstream->TimeStamp = work->timestamp;
    if (eos)
        bitstream = NULL;

    QLIST_FOREACH(work_surface, &m_session->surface_pool, next)
    {
        if (!work_surface->used && !work_surface->surface.Data.Locked) {
            break;
        }
    }
    if (work_surface == NULL) {
        DPRINTF("virtio-video: stream %d no available surface "
                     "in surface pool", stream->id);
        QLIST_FOREACH(work_surface, &m_session->surface_pool, next) {
            DPRINTF("surface:%p, used:%d, locked:%d\n", work_surface, work_surface->used, work_surface->surface.Data.Locked);
        }
        return MFX_ERR_NOT_ENOUGH_BUFFER;
    }
 

    if (stream->out.params.format != VIRTIO_VIDEO_FORMAT_NV12) {
        QLIST_FOREACH(vpp_work_surface, &m_session->vpp_surface_pool, next) {
            if (!vpp_work_surface->used &&
                !vpp_work_surface->surface.Data.Locked) {
                break;
            }
        }
        if (vpp_work_surface == NULL) {
            error_report("virtio-video: stream %d no available surface "
                         "in vpp surface pool",
                         stream->id);
            return MFX_ERR_NOT_ENOUGH_BUFFER;
        }
    }

    do {
        DPRINTF("bs:%p, input surface:%p \n", bitstream, work_surface);
        status = MFXVideoDECODE_DecodeFrameAsync(m_session->session, bitstream,
                                                 &work_surface->surface,
                                                 &out_surface, &m_frame->sync);
        DPRINTF("MFXVideoDECODE_DecodeFrameAsync return %s, out_surface:%p\n",
                virtio_video_status_to_string(status), out_surface);
        switch (status) {
        case MFX_WRN_DEVICE_BUSY:
            usleep(1000);
            break;
        case MFX_WRN_VIDEO_PARAM_CHANGED:
            DPRINTF("MFX_WRN_VIDEO_PARAM_CHANGED\n");
            MFXVideoDECODE_GetVideoParam(m_session->session, &param);
            virtio_video_msdk_stream_reset_param(stream, &param, false);
            break;
        case MFX_ERR_NONE:
            break;
        case MFX_ERR_MORE_DATA:
            // incase more data has output surface, we go through surface check
            // logic.
            break;
        case MFX_ERR_MORE_SURFACE:
            return status;
        case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
            /* these are not treated as error */
            return status;
        default:
            error_report("virtio-video: stream %d input resource %d "
                         "MFXVideoDECODE_DecodeFrameAsync failed: %d",
                         stream->id, work->resource->id, status);
            return status;
        }
    } while (status == MFX_WRN_DEVICE_BUSY);

    QLIST_FOREACH(work_surface, &m_session->surface_pool, next) {
        if (&work_surface->surface == out_surface) {
            work_surface->used = true;
            DPRINTF("set surface:%p used to true\n", work_surface);
            m_frame->surface = work_surface;
            break;
        }
    }

    if (work_surface == NULL) {
        if (out_surface != NULL) {
            error_report(
                "virtio-video: Warning: stream %d decode output surface "
                "not in surface pool",
                stream->id);

            DPRINTF("out_surface %p\n", out_surface);
            DPRINTF("bitstream %p\n", bitstream);
            DPRINTF("status %d\n", status);
            QLIST_FOREACH(work_surface, &m_session->surface_pool, next)
            {
                DPRINTF("surface in surface_pool %p\n", &work_surface->surface);
            }
        }
        return status;
    }

    if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12)
        return status;

    do {
        status = MFXVideoVPP_RunFrameVPPAsync(m_session->session,
                &work_surface->surface, &vpp_work_surface->surface,
                NULL, &m_frame->vpp_sync);
        switch (status) {
        case MFX_WRN_DEVICE_BUSY:
            usleep(1000);
            break;
        case MFX_ERR_NONE:
            break;
        case MFX_ERR_MORE_DATA:
        case MFX_ERR_MORE_SURFACE:
            /* this should not happen when doing color format conversion */
            error_report("virtio-video: BUG: stream %d color format "
                         "conversion failed with unexpected error",
                         stream->id);
            work_surface->used = false;
            return MFX_ERR_UNDEFINED_BEHAVIOR;
        default:
            error_report("virtio-video: stream %d input resource %d "
                         "MFXVideoVPP_RunFrameVPPAsync failed: %d",
                         stream->id, work->resource->id, status);
            work_surface->used = false;
            return status;
        }
    } while (status == MFX_WRN_DEVICE_BUSY);

    work_surface->used = false;
    DPRINTF("dyang23 work_surface->used:%d, surface.Data.Locked:%d", work_surface->used, work_surface->surface.Data.Locked);
    vpp_work_surface->used = true;
    m_frame->vpp_surface = vpp_work_surface;
    return MFX_ERR_NONE;
}

static mfxStatus virtio_video_decode_submit_one_work(VirtIOVideoWork *work,
    bool eos)
{
    VirtIOVideoStream *stream = work->parent;
    VirtIOVideoFrame *frame;
    MsdkSession *m_session = stream->opaque;
    MsdkFrame *m_frame;
    mfxStatus status;
    mfxBitstream *input = work->opaque;
    mfxBitstream *bitstream = &m_session->bitstream;
    bool inserted = false;

    DPRINTF("eos:%d\n", eos);

    if (work->queue_type != VIRTIO_VIDEO_QUEUE_TYPE_INPUT)
        return MFX_ERR_UNDEFINED_BEHAVIOR;

    /**
     * - CMD_RESOURCE_QUEUE requests may not share one common timestamp -
     *
     * Theoretically, we can accept that multiple requests each representing
     * part of a frame share one common timestamp, like this:
     *
     *  w1(t1) w2(t2) w3(t2) w4(t2) w5(t3): w3, w4, w5 are all valid
     *
     * And only reject uncontinuous requests with common timestamp:
     *
     *  w1(t1) w2(t2) w3(t2) w4(t3) w5(t2): w5 is invalid
     *
     * However, since we can get MFX_ERR_MORE_DATA even when the input
     * bitstream contains a complete frame, we cannot determine whether an
     * incoming buffer with the same timestamp as previous buffers is part of
     * previous frame or contains a new frame.
     *
     * Suppose we have 3 frames in 3 buffers, all with the same timestamp:
     *
     *  w1(t1) w2(t2) w3(t2) w4(t2) w5(t3): w2, w3, w4 each contain a frame
     *
     * The ideal frame queue arrangement is:
     *
     *  f1(t1) f2(t2) f3(t2) f4(t2) f5(t3)
     *
     * which at least get the timestamps correct. But it actually would be:
     *
     *  f1(t1) f2(t2) f3(t3) f4(t3) f5(t3)
     *
     * Because when we receive w2, w3 and w4, only one frame (f2) is created
     * for them. When we get frames from MediaSDK, we don't know from which
     * input bitstream it comes from, and cannot split f2 into multiple frames
     * all with timestamp=t2. When we find that there are more frames beyond
     * f3, all we can do is just creating new frames with timestamp=t3.
     *
     * Now, we explicitly prohibit the share of timestamp, so that we can make
     * sure the frontend follows the spec and everything has defined semantics.
     */

    DPRINTF("decode input bs timestamp:%llu\n", (unsigned long long)work->timestamp);

    if (!m_session->input_accepted) {
        virtio_video_msdk_bitstream_append(bitstream, input);
    }

    /* the frontend uses an empty buffer to drain the stream */
    bitstream->DataFlag = (input->DataLength == 0 || eos) ?
                              MFX_BITSTREAM_COMPLETE_FRAME | MFX_BITSTREAM_EOS : 0;
    eos = (input->DataLength == 0 || eos) ? true : false;

    while (true) {
        m_frame = g_new0(MsdkFrame, 1);
        status = virtio_video_decode_one_frame(work, m_frame, eos);

        if (status == MFX_ERR_NOT_ENOUGH_BUFFER || (status != MFX_ERR_NONE && status != MFX_ERR_MORE_SURFACE && !eos)) {
            if (m_frame->surface) {
                error_report("%s status:%d with valid surface:%p\n", __func__,
                             status, m_frame->surface);
            } else {
                g_free(m_frame);
                break;
            }
        }
        // MFX_ERR_MORE_SURFACE, incase there is valid output surface need
        // handle.
        DPRINTF("m_frame->surface:%p\n", m_frame->surface);
        if (!m_frame->surface && !eos) {
            g_free(m_frame);
            break;
        }

        QTAILQ_FOREACH(frame, &stream->pending_frames, next) {
            if (frame->opaque == NULL) {
                break;
            }
        }
        if (frame == NULL) {
            if (inserted) {
                warn_report("virtio-video: stream %d generated too many "
                            "frames, more than the number of input buffers",
                            stream->id);
            }
            frame = g_new0(VirtIOVideoFrame, 1);
            QTAILQ_INSERT_TAIL(&stream->pending_frames, frame, next);
            inserted = true;
        }
        frame->timestamp = eos && (status == MFX_ERR_MORE_DATA) ? 0 : (work->timestamp ? work->timestamp : 1);
        frame->opaque = m_frame;
        if (eos && status == MFX_ERR_MORE_DATA)
            break;
    }

    bitstream->DataFlag = 0;
    qemu_event_set(&m_session->output_notifier);
    return status;
}

static void virtio_video_decode_retrieve_one_frame(VirtIOVideoFrame *frame,
                                                   VirtIOVideoWork *work)
{
    VirtIOVideoStream *stream = work->parent;
    MsdkSession *m_session = stream->opaque;
    MsdkFrame *m_frame = frame->opaque;
    mfxStatus status;
    int ret;
    DPRINTF("\n");

    /* indicate that this work is currently being processed */
    work->opaque = m_frame;
    qemu_mutex_unlock(&stream->mutex);
    do {
        if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12) {
            status =
                MFXVideoCORE_SyncOperation(m_session->session, m_frame->sync,
                                           VIRTIO_VIDEO_MSDK_TIME_TO_WAIT);
        } else {
            status = MFXVideoCORE_SyncOperation(m_session->session,
                                                m_frame->vpp_sync,
                                                VIRTIO_VIDEO_MSDK_TIME_TO_WAIT);
        }
        /* work is cancelled by CMD_QUEUE_CLEAR or CMD_RESOURCE_DESTROY_ALL */
        if (work->flags == VIRTIO_VIDEO_BUFFER_FLAG_ERR) {
            error_report("%s with work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR",
                         __func__);
            qemu_mutex_lock(&stream->mutex);
            virtio_video_work_done(work);
            return;
        }
        /* the output work should be reused when input queue is cleared */
        if (status == MFX_WRN_IN_EXECUTION &&
            (stream->state == STREAM_STATE_INPUT_PAUSED ||
             stream->state == STREAM_STATE_TERMINATE)) {
            qemu_mutex_lock(&stream->mutex);
            work->opaque = NULL;
            error_report("%s sync wait failed, need clear buffer\n", __func__);
            return;
        }
    } while (status == MFX_WRN_IN_EXECUTION);

    if (status != MFX_ERR_NONE) {
        ret = -1;
        error_report("virtio-video-dec-output/%d MFXVideoCORE_SyncOperation "
                     "failed: %d",
                     stream->id, status);
    } else {
        if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12)
            ret = m_frame->surface->surface.Data.Corrupted;
        else
            ret = m_frame->vpp_surface->surface.Data.Corrupted;
        if (ret != 0) {
            DPRINTF("virtio-video-dec-output/%d frame (timestamp=%luns) "
                    "corrupted: %d\n",
                    stream->id, frame->timestamp, ret);
        }
    }

    /* It's better to output something, even if it's corrupted. */
    if (ret != 0) {
        error_report("%s:Line %d ret: %d", __func__, __LINE__, ret);
        work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
    }

    if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12) {
        ret = virtio_video_msdk_output_surface(m_session, m_frame->surface,
                                               work->resource);
    } else {
        ret = virtio_video_msdk_output_surface(m_session, m_frame->vpp_surface,
                                               work->resource);
    }

    /* Failed to output the surface, continue with partial output or even
     * garbage data. This is still better than dropping the output buffer
     * silently, which may cause deadlock in frontend. */
    if (ret < 0) {
        error_report("%s:Line %d ret: %d", __func__, __LINE__, ret);
        work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
    }

    qemu_mutex_lock(&stream->mutex);
    QTAILQ_REMOVE(&stream->pending_frames, frame, next);
    if (QTAILQ_IN_USE(work, next))
        QTAILQ_REMOVE(&stream->output_work, work, next);

    if (stream->out.params.format == VIRTIO_VIDEO_FORMAT_NV12) {
        work->timestamp = m_frame->surface->surface.Data.TimeStamp;
    } else {
        work->timestamp = m_frame->vpp_surface->surface.Data.TimeStamp;
    }

    virtio_video_msdk_uninit_frame(frame);
    virtio_video_work_done(work);
    qemu_event_set(&m_session->input_notifier);
    return;
}

static void *virtio_video_decode_input_thread(void *arg)
{
    VirtIOVideoStream *stream = arg;
    VirtIOVideoWork *work;
    MsdkSession *m_session = stream->opaque;
    mfxBitstream *bitstream;
    mfxStatus status;
    bool eos;

    DPRINTF("virtio-video-dec-input/%d started\n", stream->id);

    while (true) {
        qemu_mutex_lock(&stream->mutex);
        switch (stream->state) {
        case STREAM_STATE_INIT:
        case STREAM_STATE_INPUT_PAUSED:
            qemu_mutex_unlock(&stream->mutex);
            qemu_event_wait(&m_session->input_notifier);
            qemu_event_reset(&m_session->input_notifier);
            continue;
        case STREAM_STATE_RUNNING:
        case STREAM_STATE_DRAIN:
            if (QTAILQ_EMPTY(&stream->input_work)) {
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_wait(&m_session->input_notifier);
                qemu_event_reset(&m_session->input_notifier);
                continue;
            }

            /* Although not specified in spec, we believe it's necessary to
             * drain the stream on STREAM_DRAIN. */
            work = QTAILQ_FIRST(&stream->input_work);
            eos = stream->state == STREAM_STATE_DRAIN &&
                  work == QTAILQ_LAST(&stream->input_work);
            status = virtio_video_decode_submit_one_work(work, eos);
            m_session->input_accepted = true;

            /* waiting for a free slot in surface pool */
            if (status == MFX_ERR_NOT_ENOUGH_BUFFER) {
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_wait(&m_session->input_notifier);
                qemu_event_reset(&m_session->input_notifier);
                continue;
            }

            m_session->input_accepted = false;
            work->timestamp = 0;
            if (status != MFX_ERR_MORE_DATA) {
                error_report("%s:Line %d status: %s\n", __func__, __LINE__,
                             virtio_video_status_to_string(status));
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
            }
            QTAILQ_REMOVE(&stream->input_work, work, next);
            bitstream = work->opaque;
            if (bitstream->Data)
                g_free(bitstream->Data);
            g_free(work->opaque);
            if (eos)
                g_free(work);
            else
                virtio_video_work_done(work);
            break;
        case STREAM_STATE_TERMINATE:
            qemu_mutex_unlock(&stream->mutex);
            goto done;
        default:
            break;
        }
        qemu_mutex_unlock(&stream->mutex);
    }

done:
    DPRINTF("virtio-video-dec-input/%d exited normally\n", stream->id);
    qemu_event_set(&m_session->input_notifier);
    return NULL;
}

#if defined VIRTIO_VIDEO_MSDK_DEC_DEBUG || defined DEBUG_VIRTIO_VIDEO_ALL
static int virtio_video_decode_valid_surfaces(VirtIOVideoStream *stream)
{
    VirtIOVideoFrame *frame;
    int i = 0;

    QTAILQ_FOREACH(frame, &stream->pending_frames, next)
    {
        if (frame->opaque != NULL)
            i++;
    }

    return i;
}
#endif

static void *virtio_video_decode_thread(void *arg)
{
    VirtIOVideoStream *stream = arg;
    VirtIOVideo *v = stream->parent;
    VirtIOVideoCmd *cmd = &stream->inflight_cmd;
    VirtIOVideoWork *work, *tmp_work;
    VirtIOVideoFrame *frame, *tmp_frame;
    MsdkSession *m_session = stream->opaque;
    mfxStatus status;
    uint32_t stream_id = stream->id;
    mfxBitstream *bitstream;
    bool eos;

    DPRINTF("virtio-video-dec-output/%d started\n", stream_id);
    object_ref(OBJECT(v));

    while (true) {
        qemu_mutex_lock(&stream->mutex);
        switch (stream->state) {
        case STREAM_STATE_INIT:
            /* waiting for the initial request which contains the header */
            if (QTAILQ_EMPTY(&stream->input_work)) {
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_wait(&m_session->output_notifier);
                qemu_event_reset(&m_session->output_notifier);
                continue;
            }

            work = QTAILQ_FIRST(&stream->input_work);
            status = virtio_video_decode_parse_header(work);
            if (status != MFX_ERR_NONE) {
                if (status != MFX_ERR_MORE_DATA) {
                    error_report("%s:Line %d status: %d", __func__, __LINE__,
                                 status);
                    work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                }
                QTAILQ_REMOVE(&stream->input_work, work, next);
                bitstream = work->opaque;
                if (bitstream->Data)
                    g_free(bitstream->Data);
                g_free(work->opaque);
                virtio_video_work_done(work);

                /*
                 * When there's no further buffers, the stream drain cmd should
                 * be cancelled because the decode process hasn't been started
                 * and can never be started with a pending stream drain cmd
                 * preventing any incoming buffer.
                 */
                if (!QTAILQ_EMPTY(&stream->input_work))
                    break;
                if (cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                    virtio_video_inflight_cmd_cancel(stream);
                } else {
                    assert(cmd->cmd_type == 0);
                }
                break;
            }

            /*
             * We must generate a resolution changed event, so that the
             * frontend can know the size of output buffer and prepare them
             * accordingly. If the event is missing, the frontend will never
             * queue output buffers.
             */
            virtio_video_report_event(
                v, VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED, stream_id);

            /* the bitstream of current buffer should not be appended twice */
            m_session->input_accepted = true;

            if (cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                DPRINTF(
                    "stream:%d, state from %s->%s\n", stream_id,
                    virtio_video_stream_statu_to_string(stream->state),
                    virtio_video_stream_statu_to_string(STREAM_STATE_DRAIN));
                stream->state = STREAM_STATE_DRAIN;
                break;
            } else {
                assert(cmd->cmd_type == 0);
            }

            /* It is not allowed to change stream params from now on. */
            DPRINTF("stream:%d, state from %s->%s\n", stream_id,
                    virtio_video_stream_statu_to_string(stream->state),
                    virtio_video_stream_statu_to_string(STREAM_STATE_RUNNING));
            stream->state = STREAM_STATE_RUNNING;
            break;
        case STREAM_STATE_RUNNING:
        case STREAM_STATE_DRAIN:
            DPRINTF("output_work:%s, pending_frames:%s opaque:%p\n",
                    QTAILQ_EMPTY(&stream->output_work) ? "No" : "Yes",
                    QTAILQ_EMPTY(&stream->pending_frames) ? "No" : "Yes",
                    QTAILQ_FIRST(&stream->pending_frames) == NULL ?
                        NULL :
                        QTAILQ_FIRST(&stream->pending_frames)->opaque);
            if (QTAILQ_EMPTY(&stream->output_work) ||
                QTAILQ_EMPTY(&stream->pending_frames) ||
                QTAILQ_FIRST(&stream->pending_frames)->opaque == NULL) {
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_wait(&m_session->output_notifier);
                qemu_event_reset(&m_session->output_notifier);
                continue;
            }

            frame = QTAILQ_FIRST(&stream->pending_frames);
            work = QTAILQ_FIRST(&stream->output_work);
            eos = !frame->timestamp;
            if (!eos)
                virtio_video_decode_retrieve_one_frame(frame, work);

            DPRINTF("stream->state:%d, eos:%d\n", stream->state, eos);
            if ((stream->state != STREAM_STATE_DRAIN && stream->state != STREAM_STATE_DRAIN_PLUS_CLEAR 
                    && stream->state != STREAM_STATE_DRAIN_PLUS_CLEAR_DISTROY) || !eos)
            {
                break;
            }
            DPRINTF("stream->state:%d, eos:%d\n", stream->state, eos);
            //assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
            DPRINTF("virtio_video_decode_valid_surfaces:%d\n",
                    virtio_video_decode_valid_surfaces(stream));

            QTAILQ_REMOVE(&stream->pending_frames, frame, next);
            virtio_video_msdk_uninit_frame(frame);

            work = QTAILQ_FIRST(&stream->output_work);
            if (work)
            {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_EOS;
                QTAILQ_REMOVE(&stream->output_work, work, next);
                DPRINTF("send VIRTIO_VIDEO_BUFFER_FLAG_EOS buffer back\n");
                virtio_video_work_done(work);
            }
            else
                DPRINTF("\n");
            virtio_video_inflight_cmd_done(stream);
            if (stream->state == STREAM_STATE_DRAIN_PLUS_CLEAR || stream->state == STREAM_STATE_DRAIN_PLUS_CLEAR_DISTROY)
            {
                QTAILQ_FOREACH_SAFE(work, &stream->output_work, next, tmp_work)
                {
                    DPRINTF("flags: %d", VIRTIO_VIDEO_BUFFER_FLAG_ERR);
                    work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR; // no indicator in spec
                    QTAILQ_REMOVE(&stream->output_work, work, next);
                    if (work->opaque == NULL)
                    {
                        virtio_video_work_done(work);
                    }
                }

                QTAILQ_FOREACH(work, &stream->output_work, next)
                {
                    DPRINTF("work(%p) in flying, cannot clear\n", work);
                }
                if (stream->state == STREAM_STATE_DRAIN_PLUS_CLEAR_DISTROY)
                {
                    virtio_video_destroy_resource_list(stream, false);
                    DPRINTF("CMD_RESOURCE_DESTROY_ALL: stream %d output resources "
                            "destroyed\n",
                            stream->id);
                }
                else
                {
                    DPRINTF("CMD_QUEUE_CLEAR: stream %d output queue cleared\n",
                            stream->id);
                }
            }

            /*
             * If the guest starts decoding another bitstream, we can detect
             * that through MFXVideoDECODE_DecodeFrameAsync return value and do
             * reinitialization there.
             */
            stream->state = STREAM_STATE_RUNNING;
            break;
        case STREAM_STATE_INPUT_PAUSED:
            QTAILQ_FOREACH_SAFE(work, &stream->input_work, next, tmp_work)
            {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                error_report("%s:Line %d wok: %d", __func__, __LINE__,
                             VIRTIO_VIDEO_BUFFER_FLAG_ERR);
                QTAILQ_REMOVE(&stream->input_work, work, next);
                bitstream = work->opaque;
                if (bitstream->Data)
                    g_free(bitstream->Data);
                g_free(work->opaque);
                virtio_video_work_done(work);
            }
            QTAILQ_FOREACH_SAFE(frame, &stream->pending_frames, next, tmp_frame)
            {
                QTAILQ_REMOVE(&stream->pending_frames, frame, next);
                virtio_video_msdk_uninit_frame(frame);
            }
            m_session->bitstream.DataOffset = 0;
            m_session->bitstream.DataLength = 0;
            m_session->input_accepted = false;

            if (cmd->cmd_type == VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL) {
                virtio_video_destroy_resource_list(stream, true);
            } else {
                assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR);
            }

            virtio_video_inflight_cmd_done(stream);
            DPRINTF("stream:%d, state from %s->%s\n", stream_id,
                    virtio_video_stream_statu_to_string(stream->state),
                    virtio_video_stream_statu_to_string(STREAM_STATE_RUNNING));
            stream->state = STREAM_STATE_RUNNING;
            qemu_event_set(&m_session->input_notifier);
            break;
        case STREAM_STATE_TERMINATE:
            QTAILQ_FOREACH_SAFE(frame, &stream->pending_frames, next, tmp_frame)
            {
                QTAILQ_REMOVE(&stream->pending_frames, frame, next);
                virtio_video_msdk_uninit_frame(frame);
            }
            QTAILQ_FOREACH_SAFE(work, &stream->input_work, next, tmp_work)
            {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                error_report("%s:Line %d flags: %d", __func__, __LINE__,
                             VIRTIO_VIDEO_BUFFER_FLAG_ERR);
                QTAILQ_REMOVE(&stream->input_work, work, next);
                bitstream = work->opaque;
                if (bitstream->Data)
                    g_free(bitstream->Data);
                g_free(work->opaque);
                virtio_video_work_done(work);
            }
            QTAILQ_FOREACH_SAFE(work, &stream->output_work, next, tmp_work)
            {
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
    assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
    if (cmd->elem) {
        virtio_video_inflight_cmd_done(stream);
    }

    if (stream->out.params.format != VIRTIO_VIDEO_FORMAT_NV12) {
        MFXVideoVPP_Close(m_session->session);
    }
    MFXVideoDECODE_Close(m_session->session);
    virtio_video_msdk_unload_plugin(m_session->session,
                                    stream->in.params.format, false);
    MFXClose(m_session->session);

    /* waiting for the input thread to exit */
    qemu_event_wait(&m_session->input_notifier);
    qemu_event_destroy(&m_session->input_notifier);
    qemu_event_destroy(&m_session->output_notifier);
    virtio_video_msdk_uninit_surface_pools(m_session);
    g_free(m_session->bitstream.Data);
    if (m_session->frame_allocator)
        g_free(m_session->frame_allocator);
    g_free(m_session);

    qemu_mutex_destroy(&stream->mutex);
    g_free(stream);
    object_unref(OBJECT(v));
    DPRINTF("virtio-video-dec-output/%d exited normally\n", stream_id);
    return NULL;
}


static int virtio_video_msdk_dec_stream_terminate(VirtIOVideoStream *stream,
                                                  VirtQueueElement *elem)
{
    VirtIOVideoCmd *cmd = &stream->inflight_cmd;
    MsdkSession *m_session = stream->opaque;

    qemu_mutex_lock(&stream->mutex);
    switch (stream->state) {
    case STREAM_STATE_INIT:
        if (cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
            virtio_video_inflight_cmd_cancel(stream);
        } else {
            assert(cmd->cmd_type == 0);
        }
        break;
    case STREAM_STATE_RUNNING:
        assert(cmd->cmd_type == 0);
        break;
    case STREAM_STATE_DRAIN:
        assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
        virtio_video_inflight_cmd_cancel(stream);
        break;
    case STREAM_STATE_INPUT_PAUSED:
        assert((cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) ||
               (cmd->cmd_type == VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL));
        virtio_video_inflight_cmd_cancel(stream);
        break;
    case STREAM_STATE_TERMINATE:
        assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
        DPRINTF("stream %d already terminated\n", stream->id);
        qemu_mutex_unlock(&stream->mutex);
        return sizeof(virtio_video_cmd_hdr);
    default:
        break;
    }

    cmd->elem = elem;
    cmd->cmd_type = VIRTIO_VIDEO_CMD_STREAM_DESTROY;
    DPRINTF("stream:%d, state from %s->%s\n", stream->id,
            virtio_video_stream_statu_to_string(stream->state),
            virtio_video_stream_statu_to_string(STREAM_STATE_TERMINATE));
    stream->state = STREAM_STATE_TERMINATE;
    QLIST_REMOVE(stream, next);
    qemu_event_set(&m_session->input_notifier);
    qemu_event_set(&m_session->output_notifier);
    qemu_mutex_unlock(&stream->mutex);
    return 0;
}

size_t virtio_video_msdk_dec_stream_create(VirtIOVideo *v,
                                           virtio_video_stream_create *req,
                                           virtio_video_cmd_hdr *resp)
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
        .GPUCopy = MFX_GPUCOPY_ON,
    };

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next)
    {
        if (stream->id == resp->stream_id) {
#if 0
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
            error_report("CMD_STREAM_CREATE: stream %d already created",
                         resp->stream_id);
            // if the android crash, hang and reboot, it may not remove all
            // streams created, then after Android reboot, it will create the
            // steam with ID which existing in backend driver. But in normal
            // case, frontend driver should never double create stream with same
            // ID. We should believe frontend driver, if create stream with the
            // ID exist, we should remove the older one, then create a new.
            return len;
#endif
            virtio_video_msdk_dec_stream_terminate(stream, NULL);
        }
    }

    if (req->in_mem_type == VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT ||
        req->out_mem_type == VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT) {
        error_report("CMD_STREAM_CREATE: unsupported memory type (object)");
        return len;
    }

    QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next)
    {
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
    m_session->va_dpy = m_handle->va_handle;
    if (status != MFX_ERR_NONE) {
        error_report("CMD_STREAM_CREATE: MFXVideoCORE_SetHandle failed: %d",
                     status);
        MFXClose(m_session->session);
        g_free(m_session);
        return len;
    }

    m_session->IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    m_session->vpp_IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    if (m_session->IOPattern == MFX_IOPATTERN_OUT_VIDEO_MEMORY) {
        m_session->frame_allocator = g_new0(mfxFrameAllocator, 1);
        m_session->frame_allocator->pthis = m_session;
        m_session->frame_allocator->Alloc = virtio_video_frame_alloc;
        m_session->frame_allocator->Lock = virtio_video_frame_lock;
        m_session->frame_allocator->Unlock = virtio_video_frame_unlock;
        m_session->frame_allocator->GetHDL = virtio_video_frame_get_handle;
        m_session->frame_allocator->Free = virtio_video_frame_free;
        status = MFXVideoCORE_SetFrameAllocator(m_session->session,
                                                m_session->frame_allocator);
    } else {
        m_session->frame_allocator = NULL;
        status = MFXVideoCORE_SetFrameAllocator(m_session->session, NULL);
    }
    if (status != MFX_ERR_NONE) {
        error_report("MFXVideoCORE_SetFrameAllocator failed: %d", status);
        MFXClose(m_session->session);
        if (m_session->frame_allocator)
            g_free(m_session->frame_allocator);
        g_free(m_session);
        return len;
    }

    stream = g_new0(VirtIOVideoStream, 1);
    stream->opaque = m_session;
    stream->parent = v;

    stream->id = req->hdr.stream_id;
    stream->in.mem_type = req->in_mem_type;
    stream->out.mem_type = req->out_mem_type;
    MEMCPY_S(stream->tag, req->tag, strlen((char *)req->tag),
             strlen((char *)req->tag));

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
#ifdef OUTPUT_RGBA
    stream->out.params.format = VIRTIO_VIDEO_FORMAT_ARGB8888;
#else
     stream->out.params.format = VIRTIO_VIDEO_FORMAT_NV12;
#endif
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
    DPRINTF("plane_size:%d, wxh=%dx%d\n",
            stream->out.params.plane_formats[0].plane_size,
            stream->out.params.frame_width, stream->out.params.frame_height);
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

    DPRINTF("stream:%d, state from %s->%s\n", stream->id, 
                            virtio_video_stream_statu_to_string(stream->state),
                            virtio_video_stream_statu_to_string(STREAM_STATE_INIT));
    stream->state = STREAM_STATE_INIT;
    for (i = 0; i < VIRTIO_VIDEO_QUEUE_NUM; i++) {
        QLIST_INIT(&stream->resource_list[i]);
    }
    QTAILQ_INIT(&stream->pending_frames);
    QTAILQ_INIT(&stream->input_work);
    QTAILQ_INIT(&stream->output_work);
    qemu_mutex_init(&stream->mutex);

    qemu_event_init(&m_session->input_notifier, false);
    qemu_event_init(&m_session->output_notifier, false);
    m_session->bitstream.Data =
        g_malloc0(1024); // if 1k enough for bigger resolutions? potential risk
    m_session->bitstream.MaxLength = 1024;
    QLIST_INIT(&m_session->surface_pool);
    QLIST_INIT(&m_session->vpp_surface_pool);

    /* The output thread is not only for output, it is the main thread which
     * initializes and destroys the decode session, while the input thread is
     * just for input. */
    snprintf(thread_name, sizeof(thread_name), "virtio-video-dec-input/%d",
             stream->id);
    qemu_thread_create(&m_session->input_thread, thread_name,
                       virtio_video_decode_input_thread, stream,
                       QEMU_THREAD_DETACHED);
    snprintf(thread_name, sizeof(thread_name), "virtio-video-dec-output/%d",
             stream->id);
    qemu_thread_create(&m_session->output_thread, thread_name,
                       virtio_video_decode_thread, stream,
                       QEMU_THREAD_DETACHED);

    QLIST_INSERT_HEAD(&v->stream_list, stream, next);
    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;

    DPRINTF("CMD_STREAM_CREATE: stream %d [%s] created\n",
            stream->id, stream->tag);
    return len;
}

size_t virtio_video_msdk_dec_stream_destroy(VirtIOVideo *v,
                                            virtio_video_stream_destroy *req,
                                            virtio_video_cmd_hdr *resp,
                                            VirtQueueElement *elem)
{
    VirtIOVideoStream *stream;
    size_t len;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next)
    {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        return len;
    }

    return virtio_video_msdk_dec_stream_terminate(stream, elem);
}

/**
 * Protocol:
 *
 * @STREAM_STATE_INIT:  There can be one and only one CMD_STREAM_DRAIN pending
 *                      in @inflight_cmd, waiting for the initialization of
 *                      decode thread. The stream will then enter
 *                      STREAM_STATE_DRAIN after initialization is done.
 *
 * @STREAM_STATE_DRAIN: There is one and only one in-flight CMD_STREAM_DRAIN in
 *                      @inflight_cmd. CMD_STREAM_DRAIN cannot be in-flight in
 *                      other states.
 */
size_t virtio_video_msdk_dec_stream_drain(VirtIOVideo *v,
                                          virtio_video_stream_drain *req,
                                          virtio_video_cmd_hdr *resp,
                                          VirtQueueElement *elem)
{
    VirtIOVideoStream *stream;
    VirtIOVideoWork *work;
    VirtIOVideoCmd *cmd;
    mfxBitstream *bitstream;
    MsdkSession *m_session;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next)
    {
        if (stream->id == req->hdr.stream_id) {
            cmd = &stream->inflight_cmd;
            break;
        }
    }
    if (stream == NULL) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("CMD_STREAM_DRAIN: stream %d not found",
                     req->hdr.stream_id);
        return len;
    }
    m_session = stream->opaque;

    qemu_mutex_lock(&stream->mutex);
    switch (stream->state) {
    case STREAM_STATE_INIT:
        if (cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN)
            break;
        if (QTAILQ_EMPTY(&stream->input_work))
            break;
        /* fall through */
    case STREAM_STATE_RUNNING:
        assert(cmd->cmd_type == 0);
        cmd->elem = elem;
        cmd->cmd_type = VIRTIO_VIDEO_CMD_STREAM_DRAIN;
        if (stream->state == STREAM_STATE_RUNNING) {
            DPRINTF("stream:%d, state from %s->%s\n", stream->id,
                    virtio_video_stream_statu_to_string(stream->state),
                    virtio_video_stream_statu_to_string(STREAM_STATE_DRAIN));
            stream->state = STREAM_STATE_DRAIN;
        }
        // the DRAIN need last output buffer with VIRTIO_VIDEO_BUFFER_FLAG_EOS
        // flag. Queue a null bs to MSDK
        DPRINTF("inputwork:%d, pending_frames:%d\n",
                !QTAILQ_EMPTY(&stream->input_work),
                !QTAILQ_EMPTY(&stream->pending_frames));
        bitstream = g_new0(mfxBitstream, 1);
        bitstream->Data = NULL;
        bitstream->DataLength = 0;
        bitstream->MaxLength = 0;

        work = g_new0(VirtIOVideoWork, 1);
        work->parent = stream;
        work->elem = NULL;
        work->resource = NULL;
        work->queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
        work->timestamp = 0;
        DPRINTF("work->timestamp = req->timestamp = %lu \n",
                work->timestamp / 1000000000);
        work->opaque = bitstream;

        QTAILQ_INSERT_TAIL(&stream->input_work, work, next);
        qemu_event_set(&m_session->input_notifier);
        DPRINTF("CMD_STREAM_CREATE (async): stream %d start to drain\n",
                stream->id);
        qemu_mutex_unlock(&stream->mutex);
        return 0;
    case STREAM_STATE_DRAIN:
        assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
        break;
    case STREAM_STATE_INPUT_PAUSED:
        assert((cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) ||
               (cmd->cmd_type == VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL));
        break;
    case STREAM_STATE_TERMINATE:
        assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
        break;
    default:
        break;
    }
    DPRINTF("CMD_STREAM_DRAIN: stream %d currently unable to "
            "serve the request",
            stream->id);
    qemu_mutex_unlock(&stream->mutex);
    return len;
}

static int virtio_video_memcpy_input_buffer(VirtIOVideoResource *res,
                                            void *dest, uint32_t size)
{
    VirtIOVideoResourceSlice *slice;
    uint32_t begin = res->plane_offsets[0], end = begin + size;
    uint32_t base = 0, diff, len;
    int i;
    DPRINTF("dest:%p, size:%d\n", dest, (int)size);

    for (i = 0; i < res->num_entries[0]; i++, base += slice->page.len) {
        slice = &res->slices[0][i];
        if (begin >= base + slice->page.len)
            continue;
        /* begin >= base is always true */
        diff = begin - base;
        len = slice->page.len - diff;
        if (end <= base + slice->page.len) {
            MEMCPY_S(dest, slice->page.base + diff, size, size);
            return 0;
        } else {
            MEMCPY_S(dest, slice->page.base + diff, len, len);
            begin += len;
            size -= len;
            dest += len;
        }
    }

    if (size > 0) {
        error_report("CMD_RESOURCE_QUEUE: output buffer insufficient "
                     "to contain the frame");
        return -1;
    }

    return 0;
}

size_t virtio_video_msdk_dec_resource_queue(VirtIOVideo *v,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp,
    VirtQueueElement *elem)
{
    MsdkSession *m_session;
    VirtIOVideoStream *stream;
    VirtIOVideoCmd *cmd;
    VirtIOVideoResource *resource;
    VirtIOVideoWork *work;
    mfxBitstream *bitstream;
    size_t len;
    int i;
    int data_len = 0;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            m_session = stream->opaque;
            cmd = &stream->inflight_cmd;
            break;
        }
    }
    if (stream == NULL) {
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("CMD_RESOURCE_QUEUE: stream %d not found",
                     req->hdr.stream_id);
        return len;
    }

    qemu_mutex_lock(&stream->mutex);
    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        DPRINTF("VIRTIO_VIDEO_QUEUE_TYPE_INPUT\n");
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

        QTAILQ_FOREACH(work, &stream->input_work, next) {
            if (resource->id == work->resource->id) {
                error_report("CMD_RESOURCE_QUEUE: stream %d input resource %d "
                             "already queued, cannot be queued again",
                             stream->id, resource->id);
                qemu_mutex_unlock(&stream->mutex);
                return len;
            }
        }

        bitstream = g_new0(mfxBitstream, 1);
        data_len = 0;
        for (i = 0; i < resource->num_entries[0]; i++) {
            data_len += resource->slices[0][i].page.len;
            DPRINTF("slice[%d].len=%d\n", i, (int)resource->slices[0][i].page.len);
        }

        bitstream->Data = g_malloc0(req->data_sizes[0]);
        virtio_video_memcpy_input_buffer(resource, bitstream->Data,
                                         req->data_sizes[0]);
        bitstream->DataLength = req->data_sizes[0];
        bitstream->MaxLength = req->data_sizes[0];
        DPRINTF("intput bitstream DataLength:%d:%d, slices:%d\n",
                req->data_sizes[0], data_len, resource->num_entries[0]);
 
        work = g_new0(VirtIOVideoWork, 1);
        work->parent = stream;
        work->elem = elem;
        work->resource = resource;
        work->queue_type = req->queue_type;
        work->timestamp = req->timestamp;
        DPRINTF("work->timestamp = req->timestamp = %lu \n", work->timestamp);
        work->opaque = bitstream;

        switch (stream->state) {
        case STREAM_STATE_INIT:
            if (cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                goto out;
            } else {
                DPRINTF("cmd-type:0x%d\n", cmd->cmd_type);
                assert(cmd->cmd_type == 0);
            }

            /* let the output thread decode the header and do initialization */
            QTAILQ_INSERT_TAIL(&stream->input_work, work, next);
            qemu_event_set(&m_session->output_notifier);
            break;
        case STREAM_STATE_RUNNING:
            assert(cmd->cmd_type == 0);
            QTAILQ_INSERT_TAIL(&stream->input_work, work, next);
            qemu_event_set(&m_session->input_notifier);
            break;
        case STREAM_STATE_DRAIN:
            assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
            goto out;
        case STREAM_STATE_INPUT_PAUSED:
            assert((cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) ||
                   (cmd->cmd_type == VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL));
            goto out;
        case STREAM_STATE_TERMINATE:
            assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
        out:
            /* Return VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION */
            if (bitstream->Data)
                g_free(bitstream->Data);
            g_free(work->opaque);
            g_free(work);
            DPRINTF("CMD_RESOURCE_QUEUE: stream %d currently unable to "
                    "queue input resources\n", stream->id);
            qemu_mutex_unlock(&stream->mutex);
            return len;
        default:
            g_free(work->opaque);
            g_free(work);
            break;
        }

        DPRINTF("CMD_RESOURCE_QUEUE: stream %d queued input resource %d "
                "timestamp(ID) %lu \n", stream->id, resource->id,
                work->timestamp);
        len = 0;
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        DPRINTF("VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT\n");
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
            assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            DPRINTF("CMD_RESOURCE_QUEUE: stream %d currently unable to "
                    "queue output resources\n",
                    stream->id);
            break;
        }

        work = g_new0(VirtIOVideoWork, 1);
        work->parent = stream;
        work->elem = elem;
        work->resource = resource;
        work->queue_type = req->queue_type;

        /*
         * Output resources are just containers for decoded frames. They must
         * be paired with frames in @pending_frames.
         */
        QTAILQ_INSERT_TAIL(&stream->output_work, work, next);
        qemu_event_set(&m_session->output_notifier);

        DPRINTF("CMD_RESOURCE_QUEUE: stream %d queued output resource %d\n",
                stream->id, resource->id);
        len = 0;
        break;
    default:
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_RESOURCE_QUEUE: invalid queue type 0x%x\n",
                     req->queue_type);
        break;
    }

    qemu_mutex_unlock(&stream->mutex);
    return len;
}

/**
 * Protocol:
 *
 * @STREAM_STATE_CLEAR: In this state, there can be one in-flight
 *                      CMD_QUEUE_CLEAR or CMD_RESOURCE_DESTROY_ALL.
 *                      CMD_RESOURCE_DESTROY_ALL has higher priority and can
 *                      cancel the currently in-flight CMD_QUEUE_CLEAR.
 */
static size_t virtio_video_msdk_dec_resource_clear(VirtIOVideoStream *stream,
                                                   uint32_t queue_type,
                                                   virtio_video_cmd_hdr *resp,
                                                   VirtQueueElement *elem,
                                                   bool destroy)
{
    VirtIOVideoCmd *cmd = &stream->inflight_cmd;
    VirtIOVideoWork *work, *tmp_work;
    MsdkSession *m_session = stream->opaque;
    mfxVideoParam param = { 0 };
    mfxBitstream *bitstream;
    mfxStatus status;

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    qemu_mutex_lock(&stream->mutex);
    switch (queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        DPRINTF("virtio_video_msdk_dec_resource_clear "
                "VIRTIO_VIDEO_QUEUE_TYPE_INPUT\n");
        switch (stream->state) {
        case STREAM_STATE_INIT:
            /*
             * CMD_STREAM_DRAIN can be pending, waiting for the completion of
             * initialization phase. Then, it can do decoding of all pending
             * works in @input_work. We simply drop those works for
             * CMD_QUEUE_CLEAR and CMD_RESOURCE_DESTROY_ALL.
             */
            if (cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                virtio_video_inflight_cmd_cancel(stream);
            } else {
                assert(cmd->cmd_type == 0);
            }

            QTAILQ_FOREACH_SAFE(work, &stream->input_work, next, tmp_work)
            {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                error_report("%s:Line %d flags: %d", __func__, __LINE__,
                             VIRTIO_VIDEO_BUFFER_FLAG_ERR);
                QTAILQ_REMOVE(&stream->input_work, work, next);
                bitstream = work->opaque;
                if (bitstream->Data)
                    g_free(bitstream->Data);
                g_free(work->opaque);
                virtio_video_work_done(work);
            }
            if (destroy) {
                virtio_video_destroy_resource_list(stream, true);
                DPRINTF("CMD_RESOURCE_DESTROY_ALL: stream %d input resources "
                        "destroyed\n",
                        stream->id);
            } else {
                DPRINTF("CMD_QUEUE_CLEAR: stream %d input queue cleared\n",
                        stream->id);
            }
            break;
        case STREAM_STATE_RUNNING:
            assert(cmd->cmd_type == 0);
            DPRINTF(
                "stream:%d, state from %s->%s\n", stream->id,
                virtio_video_stream_statu_to_string(stream->state),
                virtio_video_stream_statu_to_string(STREAM_STATE_INPUT_PAUSED));

            status = MFXVideoDECODE_GetVideoParam(m_session->session, &param);
            DPRINTF("MFXVideoDECODE_GetVideoParam %s\n",
                    virtio_video_status_to_string(status));
            status = MFXVideoDECODE_Reset(m_session->session, &param);
            DPRINTF("MFXVideoDECODE_Reset %s\n",
                    virtio_video_status_to_string(status));
            // frontend will send new PPS and SPS and Iframe, after clear
            // command.
            printf("%d\n", status);
            stream->state = STREAM_STATE_INPUT_PAUSED;

        succeed:
            cmd->elem = elem;
            cmd->cmd_type = destroy ? VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL :
                                      VIRTIO_VIDEO_CMD_QUEUE_CLEAR;
            qemu_event_set(&m_session->output_notifier);

            if (destroy) {
                DPRINTF("CMD_RESOURCE_DESTROY_ALL (async): stream %d start "
                        "to destroy input resources\n",
                        stream->id);
            } else {
                DPRINTF("CMD_QUEUE_CLEAR (async): stream %d start "
                        "to clear input queue\n",
                        stream->id);
            }
            qemu_mutex_unlock(&stream->mutex);
            return 0;
        case STREAM_STATE_DRAIN:
            assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
            virtio_video_inflight_cmd_cancel(stream);
            DPRINTF(
                "stream:%d, state from %s->%s\n", stream->id,
                virtio_video_stream_statu_to_string(stream->state),
                virtio_video_stream_statu_to_string(STREAM_STATE_INPUT_PAUSED));
            stream->state = STREAM_STATE_INPUT_PAUSED;
            goto succeed;
        case STREAM_STATE_INPUT_PAUSED:
            assert((cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) ||
                   (cmd->cmd_type == VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL));
            if (destroy && cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) {
                virtio_video_inflight_cmd_cancel(stream);
                goto succeed;
            }
            goto fail;
        case STREAM_STATE_TERMINATE:
            assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            goto fail;
        default:
        fail:
            DPRINTF("%s: stream %d currently unable to serve the request",
                    destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                    stream->id);
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            break;
        }
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        DPRINTF("virtio_video_msdk_dec_resource_clear "
                "VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT \n");
        if (stream->state == STREAM_STATE_TERMINATE) {
            assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            DPRINTF("%s: stream %d currently unable to serve the request",
                    destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                    stream->id);
            break;
        }
        if (stream->state == STREAM_STATE_DRAIN)
        {
            if (destroy)
                stream->state = STREAM_STATE_DRAIN_PLUS_CLEAR_DISTROY;
            else
                stream->state = STREAM_STATE_DRAIN_PLUS_CLEAR;
        }
        else
        {
            /* release the work currently being processed on decode thread */
            QTAILQ_FOREACH_SAFE(work, &stream->output_work, next, tmp_work)
            {
                DPRINTF("flags: %d", VIRTIO_VIDEO_BUFFER_FLAG_ERR);
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR; // no indicator in spec
                QTAILQ_REMOVE(&stream->output_work, work, next);
                if (work->opaque == NULL)
                {
                    virtio_video_work_done(work);
                }
            }

            QTAILQ_FOREACH(work, &stream->output_work, next)
            {
                DPRINTF("work(%p) in flying, cannot clear\n", work);
            }
            if (destroy)
            {
                virtio_video_destroy_resource_list(stream, false);
                DPRINTF("CMD_RESOURCE_DESTROY_ALL: stream %d output resources "
                        "destroyed\n",
                        stream->id);
            }
            else
            {
                DPRINTF("CMD_QUEUE_CLEAR: stream %d output queue cleared\n",
                        stream->id);
            }
        }

        break;
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("%s: invalid queue type 0x%x",
                     destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                     queue_type);
        break;
    }

    qemu_mutex_unlock(&stream->mutex);
    return sizeof(*resp);
}

size_t virtio_video_msdk_dec_resource_destroy_all(
    VirtIOVideo *v, virtio_video_resource_destroy_all *req,
    virtio_video_cmd_hdr *resp, VirtQueueElement *elem)
{
    VirtIOVideoStream *stream;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;

    QLIST_FOREACH(stream, &v->stream_list, next)
    {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        error_report("CMD_RESOURCE_DESTROY_ALL: stream %d not found",
                     req->hdr.stream_id);
        return sizeof(*resp);
    }

    return virtio_video_msdk_dec_resource_clear(stream, req->queue_type, resp,
                                                elem, true);
}

size_t virtio_video_msdk_dec_queue_clear(VirtIOVideo *v,
                                         virtio_video_queue_clear *req,
                                         virtio_video_cmd_hdr *resp,
                                         VirtQueueElement *elem)
{
    VirtIOVideoStream *stream;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;

    QLIST_FOREACH(stream, &v->stream_list, next)
    {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        error_report("CMD_QUEUE_CLEAR: stream %d not found",
                     req->hdr.stream_id);
        return sizeof(*resp);
    }

    return virtio_video_msdk_dec_resource_clear(stream, req->queue_type, resp,
                                                elem, false);
}

size_t virtio_video_msdk_dec_get_params(VirtIOVideo *v,
                                        virtio_video_get_params *req,
                                        virtio_video_get_params_resp *resp)
{
    VirtIOVideoStream *stream;
    size_t len;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next)
    {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        error_report("CMD_GET_PARAMS: stream %d not found", req->hdr.stream_id);
        return len;
    }

    resp->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_PARAMS;
    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        MEMCPY_S(&resp->params, &stream->in.params, sizeof(resp->params),
                 sizeof(resp->params));
        DPRINTF("CMD_GET_PARAMS: reported input params\n");
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        MEMCPY_S(&resp->params, &stream->out.params, sizeof(resp->params),
                 sizeof(resp->params));
        DPRINTF("CMD_GET_PARAMS: reported output params\n");
        break;
    default:
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_GET_PARAMS: invalid queue type 0x%x",
                     req->queue_type);
        break;
    }

    return len;
}

size_t virtio_video_msdk_dec_set_params(VirtIOVideo *v,
                                        virtio_video_set_params *req,
                                        virtio_video_cmd_hdr *resp)
{
    VirtIOVideoStream *stream;
    size_t len;
    int i;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(stream, &v->stream_list, next)
    {
        if (stream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (stream == NULL) {
        error_report("CMD_SET_PARAMS: stream %d not found", req->hdr.stream_id);
        return len;
    }

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    qemu_mutex_lock(&stream->mutex);
    switch (req->params.queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        if (stream->state != STREAM_STATE_INIT) {
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            error_report("CMD_SET_PARAMS: stream %d is not allowed to change "
                         "param after decoding has started",
                         stream->id);
            break;
        }

        if (!virtio_video_format_is_codec(req->params.format)) {
            error_report("CMD_SET_PARAMS: stream %d try to set decoder "
                         "input queue format to %s",
                         stream->id,
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
                         "param after decoding has started",
                         stream->id);
        }

        if (virtio_video_format_is_codec(req->params.format)) {
            error_report("CMD_SET_PARAMS: stream %d try to set decoder "
                         "output queue format to %s",
                         stream->id,
                         virtio_video_format_name(req->params.format));
            break;
        }

        /*
         * Output parameters should be derived from input bitstream. The
         * frontend is only allowed to change pixel format.
         *
         * TODO: figure out if we should also allow setting output crop
         */
        DPRINTF("set output queue: format:%d, %s\n", req->params.format,
                virtio_video_fmt_to_string(req->params.format));
        stream->out.params.format = req->params.format;
        DPRINTF("set output queue: num_planes:%d\n", req->params.num_planes);
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

size_t
virtio_video_msdk_dec_query_control(VirtIOVideo *v,
                                    virtio_video_query_control *req,
                                    virtio_video_query_control_resp **resp)
{
    VirtIOVideoFormat *fmt;
    void *req_buf = (char *)req + sizeof(*req);
    void *resp_buf;
    size_t len = sizeof(**resp);

    switch (req->control) {
    case VIRTIO_VIDEO_CONTROL_PROFILE: {
        virtio_video_query_control_profile *query = req_buf;
        virtio_video_query_control_resp_profile *resp_profile;

        QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next)
        {
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
                         "profiles",
                         virtio_video_format_name(query->format));
            goto error;
        }

        len += sizeof(*resp_profile) + sizeof(uint32_t) * fmt->profile.num;
        *resp = g_malloc0(len);

        resp_profile = resp_buf = (char *)(*resp) + sizeof(**resp);
        resp_profile->num = fmt->profile.num;
        resp_buf += sizeof(*resp_profile);
        MEMCPY_S(resp_buf, fmt->profile.values,
               sizeof(uint32_t) * fmt->profile.num,
	       sizeof(uint32_t) * fmt->profile.num);

        DPRINTF("CMD_QUERY_CONTROL: format %s reported %d supported profiles\n",
                virtio_video_format_name(query->format), fmt->profile.num);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_LEVEL: {
        virtio_video_query_control_level *query = req_buf;
        virtio_video_query_control_resp_level *resp_level;

        QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next)
        {
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
                         "levels",
                         virtio_video_format_name(query->format));
            goto error;
        }

        len += sizeof(*resp_level) + sizeof(uint32_t) * fmt->level.num;
        *resp = g_malloc0(len);

        resp_level = resp_buf = (char *)(*resp) + sizeof(**resp);
        resp_level->num = fmt->level.num;
        resp_buf += sizeof(*resp_level);
        MEMCPY_S(resp_buf, fmt->level.values, sizeof(uint32_t) * fmt->level.num,
                 sizeof(uint32_t) * fmt->level.num);

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
                                         virtio_video_get_control *req,
                                         virtio_video_get_control_resp **resp)
{
    VirtIOVideoStream *stream;
    size_t len = sizeof(**resp);

    QLIST_FOREACH(stream, &v->stream_list, next)
    {
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
    case VIRTIO_VIDEO_CONTROL_BITRATE: {
        virtio_video_control_val_bitrate *val;

        if (stream->control.bitrate == 0)
            goto error;

        len += sizeof(*val);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(**resp);
        val->bitrate = stream->control.bitrate;
        DPRINTF("CMD_GET_CONTROL: stream %d reports bitrate = %d\n", stream->id,
                val->bitrate);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_PROFILE: {
        virtio_video_control_val_profile *val;

        if (stream->control.profile == 0)
            goto error;

        len += sizeof(*val);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(**resp);
        val->profile = stream->control.profile;
        DPRINTF("CMD_GET_CONTROL: stream %d reports profile = %d\n", stream->id,
                val->profile);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_LEVEL: {
        virtio_video_control_val_level *val;

        if (stream->control.level == 0)
            goto error;

        len += sizeof(*val);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(**resp);
        val->level = stream->control.level;
        DPRINTF("CMD_GET_CONTROL: stream %d reports level = %d\n", stream->id,
                val->level);
        break;
    }
    default:
error:
        *resp = g_malloc(sizeof(**resp));
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
        error_report("CMD_GET_CONTROL: stream %d does not support "
                     "control type 0x%x",
                     stream->id, req->control);
        break;
    }

    (*resp)->hdr.stream_id = req->hdr.stream_id;
    return len;
}

size_t virtio_video_msdk_dec_set_control(VirtIOVideo *v,
                                         virtio_video_set_control *req,
                                         virtio_video_set_control_resp *resp)
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
    virtio_video_format out_format[] = { VIRTIO_VIDEO_FORMAT_NV12,
                                         VIRTIO_VIDEO_FORMAT_ARGB8888 };
    mfxStatus status;
    mfxSession session;
    int i;

    mfxInitParam init_param = {
        .Implementation = MFX_IMPL_AUTO_ANY,
        .Version.Major = VIRTIO_VIDEO_MSDK_VERSION_MAJOR,
        .Version.Minor = VIRTIO_VIDEO_MSDK_VERSION_MINOR,
    };

    mfxVideoParam param = { 0 }, corrected_param = { 0 };

    if (virtio_video_msdk_init_handle(v)) {
        return -1;
    }

    status = MFXInitEx(init_param, &session);
    if (status != MFX_ERR_NONE) {
        error_report("MFXInitEx failed: %d", status);
        return -1;
    }

    m_handle = v->opaque;
    status = MFXVideoCORE_SetHandle(session, MFX_HANDLE_VA_DISPLAY,
                                    m_handle->va_handle);
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
            if (status == MFX_ERR_NONE ||
                status == MFX_WRN_PARTIAL_ACCELERATION) {
                w_max = corrected_param.mfx.FrameInfo.Width;
                h_max = corrected_param.mfx.FrameInfo.Height;
                break;
            }
            param.mfx.FrameInfo.Width -= VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
            param.mfx.FrameInfo.Height -=
                (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
                    VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE :
                    VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS;
        }

        /* Query the min size supported */
        param.mfx.FrameInfo.Width = VIRTIO_VIDEO_MSDK_DIMENSION_MIN;
        param.mfx.FrameInfo.Height =
            (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
                VIRTIO_VIDEO_MSDK_DIMENSION_MIN :
                VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS;
        while (param.mfx.FrameInfo.Width <= w_max &&
               param.mfx.FrameInfo.Height <= h_max) {
            status = MFXVideoDECODE_Query(session, &param, &corrected_param);
            if (status == MFX_ERR_NONE ||
                status == MFX_WRN_PARTIAL_ACCELERATION) {
                w_min = corrected_param.mfx.FrameInfo.Width;
                h_min = corrected_param.mfx.FrameInfo.Height;
                break;
            }
            param.mfx.FrameInfo.Width += VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
            param.mfx.FrameInfo.Height +=
                (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
                    VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE :
                    VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS;
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
        in_fmt_frame->frame.height.step =
            (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
                VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE :
                VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS;

        /* For decoding, frame rate may be unspecified */
        in_fmt_frame->frame.num_rates = 1;
        in_fmt_frame->frame_rates = g_new0(virtio_video_format_range, 1);
        in_fmt_frame->frame_rates[0].min = 1;
        in_fmt_frame->frame_rates[0].max = 60;
        in_fmt_frame->frame_rates[0].step = 1;

        in_fmt->desc.num_frames++;
        QLIST_INSERT_HEAD(&in_fmt->frames, in_fmt_frame, next);
        QLIST_INSERT_HEAD(&v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], in_fmt,
                          next);

        DPRINTF("Input capability %s: "
                "width [%d, %d] @%d, height [%d, %d] @%d, rate [%d, %d] @%d\n",
                virtio_video_format_name(in_format), w_min, w_max,
                in_fmt_frame->frame.width.step, h_min, h_max,
                in_fmt_frame->frame.height.step,
                in_fmt_frame->frame_rates[0].min,
                in_fmt_frame->frame_rates[0].max,
                in_fmt_frame->frame_rates[0].step);

        /* Query supported profiles */
        if (virtio_video_format_profile_range(in_format, &ctrl_min, &ctrl_max) <
            0)
            goto out;
        in_fmt->profile.values =
            g_malloc0(sizeof(uint32_t) * (ctrl_max - ctrl_min) + 1);
        for (ctrl = ctrl_min; ctrl <= ctrl_max; ctrl++) {
            param.mfx.CodecProfile = virtio_video_profile_to_msdk(ctrl);
            if (param.mfx.CodecProfile == 0)
                continue;
            status = MFXVideoDECODE_Query(session, &param, &corrected_param);
            if (status == MFX_ERR_NONE ||
                status == MFX_WRN_PARTIAL_ACCELERATION) {
                in_fmt->profile.values[in_fmt->profile.num++] =
                    param.mfx.CodecProfile;
            }
        }
        if (in_fmt->profile.num == 0)
            g_free(in_fmt->profile.values);

        /* Query supported levels */
        if (virtio_video_format_level_range(in_format, &ctrl_min, &ctrl_max) < 0)
            goto out;
        in_fmt->level.values =
            g_malloc0(sizeof(uint32_t) * (ctrl_max - ctrl_min) + 1);
        param.mfx.CodecProfile = 0;
        for (ctrl = ctrl_min; ctrl <= ctrl_max; ctrl++) {
            param.mfx.CodecLevel = virtio_video_level_to_msdk(ctrl);
            if (param.mfx.CodecLevel == 0)
                continue;
            status = MFXVideoDECODE_Query(session, &param, &corrected_param);
            if (status == MFX_ERR_NONE ||
                status == MFX_WRN_PARTIAL_ACCELERATION) {
                in_fmt->level.values[in_fmt->level.num++] =
                    param.mfx.CodecLevel;
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
        MEMCPY_S(&out_fmt_frame->frame, &in_fmt_frame->frame,
               sizeof(virtio_video_format_frame),
	       sizeof(virtio_video_format_frame));

        len = sizeof(virtio_video_format_range) * in_fmt_frame->frame.num_rates;
        out_fmt_frame->frame_rates = g_malloc0(len);
        MEMCPY_S(out_fmt_frame->frame_rates, in_fmt_frame->frame_rates, len, len);

        out_fmt->desc.num_frames++;
        QLIST_INSERT_HEAD(&out_fmt->frames, out_fmt_frame, next);
        QLIST_INSERT_HEAD(&v->format_list[VIRTIO_VIDEO_QUEUE_OUTPUT], out_fmt,
                          next);

        DPRINTF(
            "Output capability %s: "
            "width [%d, %d] @%d, height [%d, %d] @%d, rate [%d, %d] @%d\n",
            virtio_video_format_name(out_format[i]),
            out_fmt_frame->frame.width.min, out_fmt_frame->frame.width.max,
            out_fmt_frame->frame.width.step, out_fmt_frame->frame.height.min,
            out_fmt_frame->frame.height.max, out_fmt_frame->frame.height.step,
            out_fmt_frame->frame_rates[0].min,
            out_fmt_frame->frame_rates[0].max,
            out_fmt_frame->frame_rates[0].step);
    }

    QLIST_FOREACH(in_fmt, &v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next)
    {
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

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, tmp_stream)
    {
        virtio_video_msdk_dec_stream_terminate(stream, NULL);
    }

    virtio_video_msdk_uninit_handle(v);
}
