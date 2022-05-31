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
 *          Lin Shen <linx.shen@intel.com>
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "virtio-video-util.h"
#include "virtio-video-msdk.h"
#include "virtio-video-msdk-enc.h"
#include "virtio-video-msdk-util.h"
#include "mfx/mfxvideo.h"

// #define CALL_NO_DEBUG

#ifdef CALL_NO_DEBUG
static uint32_t CALL_No = 0;
#endif

// static uint32_t FRAME_NUM = 0;

#define THREAD_NAME_LEN 48
void virtio_video_msdk_enc_set_param_default(VirtIOVideoStream *stream, uint32_t coded_fmt);
int virtio_video_msdk_enc_stream_terminate(VirtIOVideoStream *pStream, VirtQueueElement *pElem, virtio_video_cmd_hdr *resp);
int virtio_video_msdk_init_encoder_stream(VirtIOVideoStream *pStream);
int virtio_video_msdk_init_enc_param(mfxVideoParam *pPara, VirtIOVideoStream *pStream);
int virtio_video_msdk_init_vpp_param(mfxVideoParam *pEncParam, mfxVideoParam *pVppParam, 
                                         VirtIOVideoStream *pStream);
mfxStatus virtio_video_encode_submit_one_frame(VirtIOVideoStream *pStream, VirtIOVideoResource *pRes, uint64_t timestamp, bool bDrain);
int virtio_video_encode_fill_input_data(MsdkFrame *pFrame, VirtIOVideoResource *pRes, 
                                        uint32_t format);
int virtio_video_encode_retrieve_one_frame(VirtIOVideoFrame *pPendingFrame, VirtIOVideoWork *pWorkOut);
void virtio_video_encode_clear_work(VirtIOVideoWork *pWork);
void virtio_video_encode_start_running(VirtIOVideoStream *pStream);
size_t virtio_video_msdk_enc_resource_clear(VirtIOVideoStream *pStream,
    uint32_t queue_type, virtio_video_cmd_hdr *resp, VirtQueueElement *elem);

#define CHECK_AND_FILL_PARAM(A, B, C) \
if (A.C != 0) { \
    B->C = A.C; \
}

static void *virtio_video_enc_overdue_stream_thread(void *arg)
{
    VirtIOVideo *v = arg;
    VirtIOVideoStream *stream = NULL;
    MsdkSession *session = NULL;
    VirtIOVideoWork *work = NULL;
    VirtIOVideoFrame *pframe = NULL, *tmpframe = NULL;
    MsdkFrame *frame = NULL;
    mfxBitstream *bs = NULL;
    uint32_t stream_id = 0;

    if (v == NULL) return NULL;

    while (v->overdue_run) {
        qemu_mutex_lock(&v->overdue_mutex);
        if (QLIST_EMPTY(&v->overdue_stream_list)) {
            qemu_mutex_unlock(&v->overdue_mutex);
            qemu_event_wait(&v->overdue_event);
            qemu_event_reset(&v->overdue_event);
            continue;
        }

        
        stream = QLIST_FIRST(&v->overdue_stream_list);
        QLIST_REMOVE(stream, next);
        qemu_mutex_unlock(&v->overdue_mutex);

        stream_id = stream->id;
        session = stream->opaque;

        // Wait input/output thread complete
        while (!session->input_thread_exited || !session->output_thread_exited) {
            qemu_event_wait(&session->inout_thread_exited);
            qemu_event_reset(&session->inout_thread_exited);
        }

        qemu_event_destroy(&session->inout_thread_exited);

        // Check whether the input/output queue is empty
        while (!QTAILQ_EMPTY(&stream->input_work)) {
            work = QTAILQ_FIRST(&stream->input_work);
            work->timestamp = 0;
            QTAILQ_REMOVE(&stream->input_work, work, next);
            virtio_video_work_done(work);
        }

        while (!QTAILQ_EMPTY(&stream->output_work)) {
            work = QTAILQ_FIRST(&stream->output_work);
            work->timestamp = 0;
            QTAILQ_REMOVE(&stream->output_work, work, next);
            virtio_video_work_done(work);
        }

        // Check whether the pending frame list is empty
        while (!QTAILQ_EMPTY(&stream->pending_frames)) {
            pframe = QTAILQ_FIRST(&stream->pending_frames);
            pframe->used = false;
            QTAILQ_REMOVE(&stream->pending_frames, pframe, next);
        }

        // Destory the mutexs and events
        qemu_mutex_destroy(&stream->mutex);
        qemu_mutex_destroy(&stream->mutex_out);
        qemu_event_destroy(&session->input_notifier);
        qemu_event_destroy(&session->output_notifier);

        // Check the mfxVideoParam is valid
        if (stream->mvp) {
            g_free(stream->mvp);
        }

        // Close mfxSession
        MFXVideoENCODE_Close(session->session);

        // Reclaim memory in the pools
        virtio_video_msdk_uninit_surface_pools(session);


        QTAILQ_FOREACH_SAFE(pframe, &session->pending_frame_pool, next, tmpframe) {
            QTAILQ_REMOVE(&session->pending_frame_pool, pframe, next);
            frame = pframe->opaque;
            bs = frame->bitstream;
            if (bs) {
                g_free(bs->Data);
                g_free(bs);
            }

            g_free(frame);
            g_free(pframe);
        }

        g_free(session);
        g_free(stream);

        DPRINTF("overdue stream %d free complete.\n", stream_id);
    }
    
    // TODO : if v is freed by main thread, how to destory the mutex and event?
    qemu_mutex_destroy(&v->overdue_mutex);
    qemu_event_destroy(&v->overdue_event);
    
    return NULL;
}



static void *virtio_video_enc_input_thread(void *arg)
{
    VirtIOVideoStream *stream = (VirtIOVideoStream *)arg;
    VirtIOVideo *v = stream->parent;
    VirtIOVideoCmd *cmd = &stream->inflight_cmd;
    VirtIOVideoWork *work = NULL;
    MsdkSession *session = stream->opaque;
    mfxStatus sts = MFX_ERR_NONE;
    uint32_t stream_id = stream->id;
    bool input_queue_clear = false;
    bool input_drain = false;

    session->input_thread_exited = false;
    DPRINTF("thread virtio-video-enc-input/%d started\n", stream_id);
    object_ref(OBJECT(v));

    while (stream->bTdRun) {
        qemu_mutex_lock(&stream->mutex);
        switch (stream->state) {
        case STREAM_STATE_INIT :
            qemu_mutex_unlock(&stream->mutex);
            qemu_event_wait(&session->input_notifier); // Wait first input resource comming and the input_notifier will be set
            qemu_event_reset(&session->input_notifier);
            if (stream->bParamSetDone) {
                sts = virtio_video_msdk_init_encoder_stream(stream);
                if (sts == MFX_ERR_NONE) {
                    stream->state = STREAM_STATE_RUNNING;
                    DPRINTF("stream %d init success. Change stream state from INIT to RUNNING.\n", stream->id);
                } else {
                    error_report("CMD_RESOURCE_QUEUE : stream %d encoder init failed."
                                " Please make sure the input and output param are correct.\n"
                                "Input :                           Output :\n"
                                "format       = %s                 format       = %s\n"
                                "frame_width  = %d                 frame_width  = %d\n"
                                "frame_height = %d                 frame_height = %d\n"
                                "min_buffers  = %d                 min_buffers  = %d\n"
                                "max_buffers  = %d                 max_buffers  = %d\n"
                                "cropX        = %d                 cropX        = %d\n"
                                "cropY        = %d                 cropY        = %d\n"
                                "cropW        = %d                 cropW        = %d\n"
                                "cropH        = %d                 cropH        = %d\n"
                                "frame_rate   = %d                 frame_rate   = %d\n"
                                "num_planes   = %d                 num_planes   = %d\n", 
                    stream->id, 
                    virtio_video_format_name(stream->in.params.format), virtio_video_format_name(stream->out.params.format), 
                    stream->in.params.frame_width,  stream->out.params.frame_width, 
                    stream->in.params.frame_height, stream->out.params.frame_height, 
                    stream->in.params.min_buffers,  stream->out.params.min_buffers, 
                    stream->in.params.max_buffers,  stream->out.params.max_buffers, 
                    stream->in.params.crop.left,    stream->out.params.crop.left, 
                    stream->in.params.crop.top,     stream->out.params.crop.top, 
                    stream->in.params.crop.width,   stream->out.params.crop.width, 
                    stream->in.params.crop.height,  stream->out.params.crop.height, 
                    stream->in.params.frame_rate,   stream->out.params.frame_rate, 
                    stream->in.params.num_planes,   stream->out.params.num_planes);
                }
            }
            continue;
            break;
        case STREAM_STATE_DRAIN :
        case STREAM_STATE_RUNNING :
            if (QTAILQ_EMPTY(&stream->input_work) && 
                stream->state == STREAM_STATE_RUNNING && 
                stream->queue_clear_type != VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_wait(&session->input_notifier);
                qemu_event_reset(&session->input_notifier);
                continue;
            }

            if (stream->queue_clear_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT)
            {
                assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR);
                input_queue_clear = true;
                input_drain = false;
            }
            else if (stream->state == STREAM_STATE_DRAIN)
            {
                assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
                input_queue_clear = false;
                input_drain = true;
            }
            else
            {
                assert(cmd->cmd_type == 0);
                input_queue_clear = false;
                input_drain = false;
            }




            do {
                if (!QTAILQ_EMPTY(&stream->input_work)) {
                    work = QTAILQ_FIRST(&stream->input_work);
                    QTAILQ_REMOVE(&stream->input_work, work, next);
                    sts = virtio_video_encode_submit_one_frame(stream, work->resource, work->timestamp, false);
                    work->timestamp = 0;
                    if ((input_queue_clear || input_drain) && QTAILQ_EMPTY(&stream->input_work)) {
                        work->flags = VIRTIO_VIDEO_BUFFER_FLAG_EOS;
                    }
                    virtio_video_work_done(work);
                }
                else {
                    break;
                }
            } while(input_queue_clear || input_drain);


            if (input_queue_clear || input_drain) {
                // Drain the MSDK
                sts = virtio_video_encode_submit_one_frame(stream, NULL, 0, true);
            }

            if (input_queue_clear)
            {
                DPRINTF("CMD_QUEUE_CLEAR : send queue_clear_resp for intput.\n");
                virtio_video_inflight_cmd_done(stream);
                stream->queue_clear_type = 0;
                stream->state = STREAM_STATE_RUNNING;
            }
            break;
        case STREAM_STATE_INPUT_PAUSED :
            break;
        case STREAM_STATE_TERMINATE :
            break;
        default :
            break;
        }
        qemu_mutex_unlock(&stream->mutex);
    }

    DPRINTF("thread virtio-video-enc-input/%d exited\n", stream_id);
    session->input_thread_exited = true;
    qemu_event_set(&session->inout_thread_exited);
    return NULL;
}

static void *virtio_video_enc_output_thread(void *arg)
{
    VirtIOVideoStream *stream = (VirtIOVideoStream *)arg;
    VirtIOVideo *v = stream->parent;
    VirtIOVideoCmd *cmd = &stream->inflight_cmd;
    VirtIOVideoWork *work = NULL;
    MsdkSession *session = stream->opaque;
    VirtIOVideoFrame *pframe = NULL;
    uint32_t stream_id = stream->id;
    bool output_queue_clear = false;
    // FILE *pTmpFile = fopen("test_output.h264", "wb+");
    // mfxBitstream *pTmpBs = NULL;
    session->output_thread_exited = false;
    DPRINTF("thread virtio-video-enc-output/%d started\n", stream_id);
    object_ref(OBJECT(v));

    while (stream->bTdRun) {
        qemu_mutex_lock(&stream->mutex_out);
        switch (stream->state) {
            case STREAM_STATE_INIT :
                qemu_mutex_unlock(&stream->mutex_out);
                qemu_event_wait(&session->output_notifier);
                qemu_event_reset(&session->output_notifier);
                continue;
                break;
            case STREAM_STATE_DRAIN :
            case STREAM_STATE_RUNNING :
                if ((QTAILQ_EMPTY(&stream->output_work) || QTAILQ_EMPTY(&stream->pending_frames)) && 
                    stream->queue_clear_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                    qemu_mutex_unlock(&stream->mutex_out);
                    qemu_event_wait(&session->output_notifier);
                    qemu_event_reset(&session->output_notifier);
                    continue;
                }

                if (cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR && 
                    stream->queue_clear_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                    output_queue_clear = true;
                } else {
                    output_queue_clear = false;
                }

                do {
                    if (QTAILQ_EMPTY(&stream->output_work)) {
                        break;
                    } else {
                        work = QTAILQ_FIRST(&stream->output_work);
                        QTAILQ_REMOVE(&stream->output_work, work, next);
                    }

                    if (QTAILQ_EMPTY(&stream->pending_frames)) {
                        pframe = NULL;
                    } else {
                        pframe = QTAILQ_FIRST(&stream->pending_frames);
                        QTAILQ_REMOVE(&stream->pending_frames, pframe, next);
                    }

                    virtio_video_encode_retrieve_one_frame(pframe, work);

                } while(output_queue_clear);

                if (output_queue_clear) {
                    DPRINTF("CMD_QUEUE_CLEAR : send queue_clear_resp for output.\n");
                    virtio_video_inflight_cmd_done(stream);
                    stream->queue_clear_type = 0;
                }
                break;
            case STREAM_STATE_TERMINATE :
                break;
            default :
                break;
        }
        qemu_mutex_unlock(&stream->mutex_out);
    }
    DPRINTF("thread virtio-video-enc-output/%d exited\n", stream_id);
    session->output_thread_exited = true;
    qemu_event_set(&session->inout_thread_exited);
    return NULL;
}


size_t virtio_video_msdk_enc_stream_create(VirtIOVideo *v,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoFormat *fmt = NULL;
    VirtIOVideoStream *stream = NULL;
    MsdkHandle *hdl = v->opaque;
    MsdkSession *session = NULL;
    mfxStatus sts = MFX_ERR_NONE;
    size_t len = 0;
    int i = 0;

    mfxInitParam param = {
        .Implementation = MFX_IMPL_AUTO_ANY, 
        .Version.Major = VIRTIO_VIDEO_MSDK_VERSION_MAJOR, 
        .Version.Minor = VIRTIO_VIDEO_MSDK_VERSION_MINOR
    };

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    // Check if the new stream ID is already in use.
    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == resp->stream_id) {
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
            error_report("CMD_STREAM_CREATE: stream %d already created in encoder", resp->stream_id);
            return len;
        }
    }

    // The OBJECT memory type is not supported yet
    if (req->in_mem_type == VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT || 
            req->out_mem_type == VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT) {
                error_report("CMD_STREAM_CREATE: unsupported memory type (object) in encoder.");
                return len;
            }
    
    // Check if the encode format is supported by encoder
    QLIST_FOREACH(fmt, &v->format_list[VIRTIO_VIDEO_QUEUE_OUTPUT], next) {
        if (fmt->desc.format == req->coded_format) {
            break;
        }
    }

    if (fmt == NULL) {
        error_report("CMD_STREAM_CREATE: unsupported codec format %s in encoder", 
            virtio_video_format_name(req->coded_format));
        return len;
    }

    session = g_new0(MsdkSession, 1);
    sts = MFXInitEx(param, &session->session);
    if (sts != MFX_ERR_NONE) {
        error_report("CMD_STREAM_CREATE: MFXInitEx failed: %d in encoder", sts);
        g_free(session);
        return len;
    }

    sts = MFXVideoCORE_SetHandle(session->session, MFX_HANDLE_VA_DISPLAY, hdl->va_handle);
    if (sts != MFX_ERR_NONE) {
        error_report("CMD_STREAM_CREATE: MFXVideoCORE_SetHandle failed: %d", sts);
        MFXClose(session->session);
        g_free(session);
        return len;
    }

    stream = g_new0(VirtIOVideoStream, 1);
    stream->opaque = session;
    stream->parent = v;

    stream->id = req->hdr.stream_id;
    stream->in.mem_type = req->in_mem_type;
    stream->out.mem_type = req->out_mem_type;
    stream->in.setted = false;
    stream->out.setted = false;
    memcpy(stream->tag, req->tag, strlen((char *)req->tag));

    virtio_video_msdk_enc_set_param_default(stream, req->coded_format);

    for (i = 0; i < VIRTIO_VIDEO_QUEUE_NUM; i++) {
        QLIST_INIT(&stream->resource_list[i]);
    }
    QTAILQ_INIT(&stream->pending_frames);
    QTAILQ_INIT(&stream->input_work);
    QTAILQ_INIT(&stream->output_work);
    qemu_mutex_init(&stream->mutex);
    qemu_mutex_init(&stream->mutex_out);
    qemu_event_init(&session->input_notifier, false);
    qemu_event_init(&session->output_notifier, false);
    qemu_event_init(&session->inout_thread_exited, false);
    QLIST_INIT(&session->surface_pool);
    QLIST_INIT(&session->vpp_surface_pool);
    QTAILQ_INIT(&session->pending_frame_pool);
    
    QLIST_INSERT_HEAD(&v->stream_list, stream, next);

    stream->bTdRun = true;
    stream->bParamSetDone = false;
    stream->state = STREAM_STATE_INIT;
    stream->queue_clear_type = 0;
    stream->mvp = NULL;

    session->input_thread_exited = false;
    session->output_thread_exited = false;
    virtio_video_encode_start_running(stream);

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;

    DPRINTF("CMD_STREAM_CREATE: stream %d [%s] format %s created in encoder\n", 
            stream->id, stream->tag, virtio_video_format_name(req->coded_format));
    
    return len;
}

size_t virtio_video_msdk_enc_stream_destroy(VirtIOVideo* v,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp, 
    VirtQueueElement *elem)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *stream = NULL;
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
        return len;
    }

    return virtio_video_msdk_enc_stream_terminate(stream, elem, resp);
}

size_t virtio_video_msdk_enc_stream_drain(VirtIOVideo *v,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp,
    VirtQueueElement *elem)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = NULL;
    size_t len = 0;
    VirtIOVideoCmd *pCmd = NULL;


    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == req->hdr.stream_id) {
            pCmd = &pStream->inflight_cmd;
            break;
        }
    }

    if (pStream == NULL) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("CMD_STREAM_DRAIN: stream %d not found.\n", req->hdr.stream_id);
        return len;
    }

    if (pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
        error_report("CMD_STREAM_DRAIN: stream %d is already processing stream_drain.\n", pStream->id);
        return len;
    }

    if (pCmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) {
        error_report("CMD_STREAM_DRAIN: stream %d is already processing queue_clear.\n", pStream->id);
        return len;
    }
    
    qemu_mutex_lock(&pStream->mutex);
    switch (pStream->state) {
    case STREAM_STATE_INIT :
    case STREAM_STATE_RUNNING : 
    case STREAM_STATE_INPUT_PAUSED : // 
        pStream->state = STREAM_STATE_DRAIN;
        pCmd->cmd_type = VIRTIO_VIDEO_CMD_STREAM_DRAIN;
        len = 0;
        break;
    case STREAM_STATE_DRAIN :
    case STREAM_STATE_TERMINATE :
        break;
    default :
        break;
    }
    qemu_mutex_unlock(&pStream->mutex);

    len = 0;
    // TODO : return stream_drain_resp async.
    return len;
}

size_t virtio_video_msdk_enc_resource_queue(VirtIOVideo *v,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp,
    VirtQueueElement *elem)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = NULL;
    MsdkSession *pSession = NULL;
    VirtIOVideoCmd *pCmd = NULL;
    VirtIOVideoResource *pRes = NULL;
    VirtIOVideoWork *pWork = NULL;
    size_t len = 0;
    uint8_t queue_type = VIRTIO_VIDEO_QUEUE_INPUT;
    bool bQueued = false;
    bool bQueueSuccess = false;
    static uint32_t input_cnt = 0;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == req->hdr.stream_id) {
            pCmd = &pStream->inflight_cmd;
            break;
        }
    }

    if (pStream == NULL) {
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("CMD_RESOURCE_QUEUE: stream %d not found.\n", req->hdr.stream_id);
        return len;
    }

    if (pCmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) {
        error_report("CMD_RESOURCE_QUEUE: stream %d is already processing queue_clear.\n", pStream->id);
        return len;
    }

    pSession = pStream->opaque;

    // Check whether the resource is valid
    queue_type = req->queue_type;
    QLIST_FOREACH(pRes, &pStream->resource_list[queue_type], next) {
        if (pRes->id == req->resource_id) {
            break;
        }
    }
    if (pRes == NULL) {
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
        error_report("stream %d resource %d for %d(0 - input | 1 - output) not found\n", pStream->id, req->resource_id, queue_type);
        return len;
    }

    // When the resource_queue comming, means the encode params set done.
    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT :
        if (!virtio_video_format_is_valid(pStream->in.params.format, req->num_data_sizes)) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
            error_report("CMD_RESOURCE_QUEUE: stream %d try to queue "
                    "a resource with num_data_sizes=%d for input queue "
                    "whose format is %s", pStream->id, req->num_data_sizes,
                    virtio_video_format_name(pStream->in.params.format));
            break;
        }

        if (pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
            error_report("CMD_RESOURCE_QUEUE: stream %d is already processing stream_drain.\n", pStream->id);
            return len;
        }

        qemu_mutex_lock(&pStream->mutex);
        // Check whether the resource is already queued
        QTAILQ_FOREACH(pWork, &pStream->input_work, next) {
            if (pRes->id == pWork->resource->id) {
                bQueued = true;
                break;
            }
        }

        if (bQueued) {
            error_report("CMD_RESOURCE_QUEUE: stream %d input resource %d "
                             "already queued, cannot be queued again", pStream->id, pRes->id);
            qemu_mutex_unlock(&pStream->mutex);
            return len;
        }

        DPRINTF("The Input Cnt is %d.\n", input_cnt++);
        switch (pStream->state) {
        case STREAM_STATE_INIT :
        case STREAM_STATE_RUNNING : // Only INIT and RUNNING can accept new input work
            pStream->bParamSetDone = true;
            assert(pCmd->cmd_type == 0);
            pWork = g_new0(VirtIOVideoWork, 1);
            pWork->parent = pStream;
            pWork->elem = elem;
            pWork->resource = pRes;
            pWork->queue_type = req->queue_type;
            pWork->timestamp = req->timestamp;

            QTAILQ_INSERT_TAIL(&pStream->input_work, pWork, next);
            qemu_event_set(&pSession->input_notifier);
            bQueueSuccess = true;
            break;
        case STREAM_STATE_DRAIN:
            assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
            // Stream will not accept new input work under STREAM_STATE_DRAIN.
            bQueueSuccess = false;
            break;
        case STREAM_STATE_INPUT_PAUSED:
            assert((pCmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) ||
                   (pCmd->cmd_type == VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL));
            bQueueSuccess = false;
            break;
        case STREAM_STATE_TERMINATE:
            assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            bQueueSuccess = false;
            break;
        default:
            break;
        }
        
        if (!bQueueSuccess) {
            DPRINTF("CMD_RESOURCE_QUEUE: stream %d currently unable to queue input resources\n", pStream->id);
        } else {
            len = 0;
        }
        qemu_mutex_unlock(&pStream->mutex);
    break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT :
        if (!virtio_video_format_is_valid(pStream->out.params.format,
                                          req->num_data_sizes)) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
            error_report("CMD_RESOURCE_QUEUE: stream %d try to queue "
                         "a resource with num_data_sizes=%d for output queue "
                         "whose format is %s", pStream->id, req->num_data_sizes,
                         virtio_video_format_name(pStream->out.params.format));
            break;
        }

        /* Only input resources have timestamp assigned. */
        if (req->timestamp != 0) {
            error_report("CMD_RESOURCE_QUEUE: stream %d try to assign "
                         "timestamp 0x%llx to output resource, which is "
                         "not allowed", pStream->id, req->timestamp);
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
           //  break;
        }

        qemu_mutex_lock(&pStream->mutex_out);
        QTAILQ_FOREACH(pWork, &pStream->output_work, next) {
            if (pRes->id == pWork->resource->id) {
                error_report("CMD_RESOURCE_QUEUE: stream %d output resource %d "
                             "already queued, cannot be queued again", pStream->id, pRes->id);
                qemu_mutex_unlock(&pStream->mutex_out);
                return len;
            }
        }
        if (pStream->state == STREAM_STATE_TERMINATE) {
            assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            qemu_mutex_unlock(&pStream->mutex_out);
            DPRINTF("CMD_RESOURCE_QUEUE: stream %d currently unable to "
                    "queue output resources\n", pStream->id);
            return len;
        }

        pWork = g_new0(VirtIOVideoWork, 1);
        pWork->parent = pStream;
        pWork->elem = elem;
        pWork->resource = pRes;
        pWork->queue_type = req->queue_type;

        QTAILQ_INSERT_TAIL(&pStream->output_work, pWork, next);
        qemu_event_set(&pSession->output_notifier);
        qemu_mutex_unlock(&pStream->mutex_out);

        // DPRINTF("CMD_RESOURCE_QUEUE : stream %d queued output resource %d\n", pStream->id, pRes->id);
        len = 0;
        break;
    default :
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_RESOURCE_QUEUE : invalid queue type 0x%x\n", req->queue_type);
    break;
    }

    // DPRINTF("CMD_RESOURCE_QUEUE : resource %d processed complete\n", req->resource_id);
    return len;
}

size_t virtio_video_msdk_enc_resource_destroy_all(VirtIOVideo *v,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp, 
    VirtQueueElement *elem)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = NULL;
    size_t len = sizeof(*resp);

    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == req->hdr.stream_id) {
            break;
        }
    }
    resp->stream_id = req->hdr.stream_id;
    if (pStream == NULL) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("CMD_RESOURCE_DESTROY_ALL : stream %d not found", req->hdr.stream_id);
        return len;
    }


    return virtio_video_msdk_enc_resource_clear(pStream, req->queue_type, 
                                                resp, elem);
}

size_t virtio_video_msdk_enc_queue_clear(VirtIOVideo *v,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp, 
    VirtQueueElement *elem)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *stream = NULL;
    MsdkSession *session = NULL;
    VirtIOVideoCmd *cmd = NULL;
    size_t len = sizeof(*resp);
    QemuMutex *mutex = NULL;
    QemuEvent *event = NULL;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    QLIST_FOREACH(stream, &v->stream_list, next) {
        if (stream->id == req->hdr.stream_id) {
            cmd = &stream->inflight_cmd;
            session = stream->opaque;
            break;
        }
    }

    if (stream == NULL) {
        error_report("CMD_QUEUE_CLEAR : stream %d not found.\n", req->hdr.stream_id);
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        return len;
    }

    if (cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) {
        error_report("CMD_QUEUE_CLEAR : stream %d is already processing queue_clear.\n", stream->id);
        return len;
    }

    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT :
        mutex = &stream->mutex;
        event = &session->input_notifier;
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT :
        mutex = &stream->mutex_out;
        event = &session->output_notifier;
        break;
    }

    if (mutex && event) {
        qemu_mutex_lock(mutex);
        cmd->cmd_type = VIRTIO_VIDEO_CMD_QUEUE_CLEAR;
        stream->queue_clear_type = req->queue_type;
        cmd->elem = elem;
        qemu_event_set(event);
        qemu_mutex_unlock(mutex);
    }

    return 0;
}

size_t virtio_video_msdk_enc_get_params(VirtIOVideo *v,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = NULL;
    size_t len = 0;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (pStream == NULL) {
        error_report("CMD_GET_PARAMS : stream %d not found in encoder\n", req->hdr.stream_id);
        return len;
    }

    resp->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_PARAMS;
    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT :
        memcpy(&resp->params, &pStream->in.params, sizeof(resp->params));
        // DPRINTF("CMD_GET_PARAMS : reported input params\n");
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT :
        memcpy(&resp->params, &pStream->out.params, sizeof(resp->params));
        // DPRINTF("CMD_GET_PARAMS : reported output params\n");
        break;
    default :
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_GET_PARAMS : invalid queue type 0x%x", req->queue_type);
        break;
    }
    return len;
}

size_t virtio_video_msdk_enc_set_params(VirtIOVideo *v,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = NULL;
    size_t len = sizeof(*resp);
    // mfxStatus sts = MFX_ERR_NONE;
    virtio_video_params *pPara = NULL;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;

    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == req->hdr.stream_id) {
            break;
        }
    }
    if (pStream == NULL) {
        error_report("CMD_SET_PARAMS: stream %d not found", req->hdr.stream_id);
        return len;
    }

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    qemu_mutex_lock(&pStream->mutex);
    switch (req->params.queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT :
        // DPRINTF("Set input params for stream %d \n", pStream->id);
        if (pStream->state != STREAM_STATE_INIT) {
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            error_report("CMD_SET_PARAMS: stream %d is not allowed to change "
                        "param after encoding has started", pStream->id);
            break;
        }
        else {
            pPara = &pStream->in.params;
        }
    break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT :
        // DPRINTF("Set output params for stream %d \n", pStream->id);
        if (pStream->state != STREAM_STATE_INIT) {
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            error_report("CMD_SET_PARAMS: stream %d is not allowed to change "
                         "param after encoding has started", pStream->id);
            break;
        }
        else {
            pPara = &pStream->out.params;
        }
    break;
    default :
    break;
    }

    if (pPara != NULL) {
         DPRINTF(
                "Current Params:                    Incoming Params:\n"
                "format       = %s       <-         format        = %s\n"
                "frame_width  = %d       <-         frame_width   = %d\n"
                "frame_height = %d       <-         frame_height  = %d\n"
                "min_buffers  = %d       <-         min_buffers   = %d\n"
                "max_buffers  = %d       <-         max_buffers   = %d\n"
                "cropX        = %d       <-         cropX         = %d\n"
                "cropY        = %d       <-         cropY         = %d\n"
                "cropW        = %d       <-         cropW         = %d\n"
                "cropH        = %d       <-         cropH         = %d\n"
                "frame_rate   = %d       <-         frame_rate    = %d\n"
                "num_planes   = %d       <-         num_planes    = %d\n" 
                "plane_format[0].stride = %d  <-    stride        = %d\n"
                "plane_format[0].plane_size = %d  <- plane_size   = %d\n",
                virtio_video_format_name(pPara->format), virtio_video_format_name(req->params.format), 
                pPara->frame_width,  req->params.frame_width, 
                pPara->frame_height, req->params.frame_height, 
                pPara->min_buffers,  req->params.min_buffers, 
                pPara->max_buffers,  req->params.max_buffers, 
                pPara->crop.left  ,  req->params.crop.left, 
                pPara->crop.top,     req->params.crop.top, 
                pPara->crop.width ,  req->params.crop.width, 
                pPara->crop.height,  req->params.crop.height, 
                pPara->frame_rate ,  req->params.frame_rate, 
                pPara->num_planes,   req->params.num_planes,
                pPara->plane_formats[0].stride, req->params.plane_formats[0].stride,
                pPara->plane_formats[0].plane_size, req->params.plane_formats[0].plane_size);

        CHECK_AND_FILL_PARAM(req->params, pPara, format);
        CHECK_AND_FILL_PARAM(req->params, pPara, frame_width);
        CHECK_AND_FILL_PARAM(req->params, pPara, frame_height);
        CHECK_AND_FILL_PARAM(req->params, pPara, min_buffers);
        CHECK_AND_FILL_PARAM(req->params, pPara, max_buffers);
        CHECK_AND_FILL_PARAM(req->params, pPara, crop.left);
        CHECK_AND_FILL_PARAM(req->params, pPara, crop.top);
        CHECK_AND_FILL_PARAM(req->params, pPara, crop.width);
        CHECK_AND_FILL_PARAM(req->params, pPara, crop.height);
        CHECK_AND_FILL_PARAM(req->params, pPara, frame_rate);
        CHECK_AND_FILL_PARAM(req->params, pPara, num_planes);
        pPara->plane_formats[0] = req->params.plane_formats[0];

        if (req->params.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
            virtio_video_param_fixup(pPara);
        }
    }

    qemu_mutex_unlock(&pStream->mutex);
    return len;
}

size_t virtio_video_msdk_enc_query_control(VirtIOVideo *v,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoFormat *pFmt = NULL;
    void *pReqBuf = (char *)req + sizeof(*req);
    void *pRespBuf = NULL;
    size_t len = sizeof(**resp);
    bool bSuccess = true;

    switch (req->control) {
    case VIRTIO_VIDEO_CONTROL_PROFILE:
    {
        virtio_video_query_control_profile *pQuery = pReqBuf;
        virtio_video_query_control_resp_profile *pRespProfile = NULL;

        QLIST_FOREACH(pFmt, &v->format_list[VIRTIO_VIDEO_QUEUE_OUTPUT], next) {
            if (pFmt->desc.format == pQuery->format) {
                break;
            }
        }
        if (pFmt == NULL) {
            error_report("CMD_QUERY_CONTROL: format %s:%d not supported in encoder",
                         virtio_video_format_name(pQuery->format), pQuery->format);
            bSuccess = false;
            break;
        }
        if (pFmt->profile.num == 0) {
            error_report("CMD_QUERY_CONTROL: format %s does not support in encoder"
                         "profiles", virtio_video_format_name(pQuery->format));
            bSuccess = false;
            break;
        }

        len += sizeof(*pRespProfile) + sizeof(uint32_t) * pFmt->profile.num;
        *resp = g_malloc0(len);

        pRespProfile = pRespBuf = (char *)(*resp) + sizeof(**resp);
        pRespProfile->num = pFmt->profile.num;
        pRespBuf += sizeof(*pRespProfile);
        memcpy(pRespBuf, pFmt->profile.values,
               sizeof(uint32_t) * pFmt->profile.num);

        DPRINTF("CMD_QUERY_CONTROL: format %s reported %d supported profiles in encoder \n",
                virtio_video_format_name(pQuery->format), pFmt->profile.num);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_LEVEL:
    {
        virtio_video_query_control_level *pQuery = pReqBuf;
        virtio_video_query_control_resp_level *pRespLevel = NULL;

        QLIST_FOREACH(pFmt, &v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next) {
            if (pFmt->desc.format == pQuery->format) {
                break;
            }
        }
        if (pFmt == NULL) {
            error_report("CMD_QUERY_CONTROL: format %s:%d not supported in encoder",
                         virtio_video_format_name(pQuery->format), pQuery->format);
            bSuccess = false;
            break;
        }
        if (pFmt->level.num == 0) {
            error_report("CMD_QUERY_CONTROL: format %s does not support "
                         "levels in encoder", virtio_video_format_name(pQuery->format));
            bSuccess = false;
            break;
        }

        len += sizeof(*pRespLevel) + sizeof(uint32_t) * pFmt->level.num;
        *resp = g_malloc0(len);

        pRespLevel = pRespBuf = (char *)(*resp) + sizeof(**resp);
        pRespLevel->num = pFmt->level.num;
        pRespBuf += sizeof(*pRespLevel);
        memcpy(pRespBuf, pFmt->level.values, sizeof(uint32_t) * pFmt->level.num);

        DPRINTF("CMD_QUERY_CONTROL: format %s reported %d supported levels in encoder\n",
                virtio_video_format_name(pQuery->format), pFmt->level.num);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_BITRATE:
        error_report("CMD_QUERY_CONTROL: virtio-video-enc does not support "
                     "bitrate");
        bSuccess = false;
        break;
    default:
        error_report("CMD_QUERY_CONTROL: unsupported control type 0x%x in encoder",
                     req->control);
        bSuccess = false;
        break;
    }

    if (bSuccess) {
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL;
        (*resp)->hdr.stream_id = req->hdr.stream_id;
        return len;
    } else {
        *resp = g_malloc(sizeof(virtio_video_query_control_resp));
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
        (*resp)->hdr.stream_id = req->hdr.stream_id;
        return sizeof(**resp);
    }
}

size_t virtio_video_msdk_enc_get_control(VirtIOVideo *v,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = NULL;
    size_t len = sizeof(**resp);
    bool bSuccess = true;
    virtio_video_control_type t = req->control;

    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == req->hdr.stream_id) {
            break;
        }
    }

    if (pStream == NULL) {
        *resp = g_malloc(sizeof(**resp));
        (*resp)->hdr.stream_id = req->hdr.stream_id;
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("CMD_GET_CONTROL : stream %d not found", req->hdr.stream_id);
        return len;
    }

    switch (t) {
    case VIRTIO_VIDEO_CONTROL_BITRATE :
    {
        virtio_video_control_val_bitrate *val = NULL;
        len += sizeof(virtio_video_control_val_bitrate);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(**resp);
        val->bitrate = pStream->control.bitrate;
        DPRINTF("CMD_GET_CONTROL: stream %d reports bitrate = %d\n",
                pStream->id, val->bitrate);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_PROFILE:
    {
        virtio_video_control_val_profile *val = NULL;
        len += sizeof(virtio_video_control_val_profile);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(**resp);
        val->profile = pStream->control.profile;
        DPRINTF("CMD_GET_CONTROL: stream %d reports profile = %d\n",
                pStream->id, val->profile);
        break;
    }
    case VIRTIO_VIDEO_CONTROL_LEVEL:
    {
        virtio_video_control_val_level *val = NULL;
        len += sizeof(virtio_video_control_val_level);
        *resp = g_malloc0(len);
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;

        val = (void *)(*resp) + sizeof(**resp);
        val->level = pStream->control.level;
        DPRINTF("CMD_GET_CONTROL: stream %d reports level = %d\n",
                pStream->id, val->level);
        break;
    }
    default:
        bSuccess = false;
        break;
    }

    if (!bSuccess) {
        *resp = g_malloc(sizeof(**resp));
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
        error_report("CMD_GET_CONTROL: stream %d does not support "
                     "control type 0x%x", pStream->id, req->control);
    }

    (*resp)->hdr.stream_id = req->hdr.stream_id;
    return len;
}

size_t virtio_video_msdk_enc_set_control(VirtIOVideo *v,
    virtio_video_set_control *req, virtio_video_set_control_resp *resp)
{
    size_t len = sizeof(*resp);
    VirtIOVideoStream *pStream = NULL;
    bool bSuccess = true;
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == req->hdr.stream_id) {
            break;
        }
    }
    resp->hdr.stream_id = req->hdr.stream_id;

    if (pStream == NULL) {
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("CMD_SET_CONTROL : stream %d not found", req->hdr.stream_id);
        return len;
    }

    switch (req->control) {
    case VIRTIO_VIDEO_CONTROL_BITRATE : {
        virtio_video_control_val_bitrate *pBr = (virtio_video_control_val_bitrate *)((char *)req + sizeof(*req));
        if (pBr->bitrate != 0) {
            pStream->control.bitrate = pBr->bitrate;
            DPRINTF("CMD_SET_CONTROL - set stream %d's bitrate to %d\n", 
                    pStream->id, pBr->bitrate);
        } else {
            bSuccess = false;
        }
        break;
    }
    case VIRTIO_VIDEO_CONTROL_PROFILE : {
        virtio_video_control_val_profile *pPf = (virtio_video_control_val_profile *)((char *)req + sizeof(*req));
        if (pPf->profile != 0) {
            pStream->control.profile = pPf->profile;
            DPRINTF("CMD_SET_CONTROL - set stream %d's profile to %d\n",
                    pStream->id, pPf->profile);
        } else {
            bSuccess = false;
        }
        break;
    }
    case VIRTIO_VIDEO_CONTROL_LEVEL : {
        virtio_video_control_val_level *pLv = (virtio_video_control_val_level *)((char *)req + sizeof(*req));
        if (pLv->level != 0) {
            pStream->control.level = pLv->level;
            DPRINTF("CMD_SET_CONTROL - set stream %d's level to %d\n",
                    pStream->id, pLv->level);
        } else {
            bSuccess = false;
        }
        break;
    }
    case VIRTIO_VIDEO_CONTROL_FORCE_KEYFRAME:
        // do nothing
        DPRINTF("CMD_SET_CONTROL - set stream %u force keyframe\n", pStream->id);
        break;
    default:
        bSuccess = false;
        break;
    }

    if (bSuccess) {
        resp->hdr.type = VIRTIO_VIDEO_RESP_OK_NODATA;
        DPRINTF("CMD_SET_CONTROL - Set stream %d's control %d success\n",
                pStream->id, req->control);
    } else {
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
        DPRINTF("CMD_SET_CONTROL - Set stream %d's control %d failed\n",
                pStream->id, req->control);
    }
    
    return len;
}


int virtio_video_init_msdk_enc(VirtIOVideo *v)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    MsdkHandle *hdl = NULL;
    mfxStatus sts = MFX_ERR_NONE;
    mfxSession mfx_session;


    mfxInitParam par = {
        .Implementation = MFX_IMPL_AUTO_ANY,
        .Version.Major = VIRTIO_VIDEO_MSDK_VERSION_MAJOR,
        .Version.Minor = VIRTIO_VIDEO_MSDK_VERSION_MINOR,
    };

    VirtIOVideoFormat *in_fmt = NULL, *out_fmt = NULL;
    VirtIOVideoFormatFrame *in_fmt_frame = NULL;
    VirtIOVideoFormatFrame *out_fmt_frame = NULL;
    virtio_video_format in_format[] = {VIRTIO_VIDEO_FORMAT_NV12};
    virtio_video_format out_format;
    uint32_t in_fmt_nums = sizeof(in_format) / sizeof(virtio_video_format);
    uint32_t out_fmt_nums = 0;
    mfxVideoParam param = {0}, corrected_param = {0};

    // DPRINTF("Init handle\n");
    if (virtio_video_msdk_init_handle(v)) {
        return -1;
    }

    // DPRINTF("MFXInitEx\n");
    sts = MFXInitEx(par, &mfx_session);
    if (sts != MFX_ERR_NONE) {
        error_report("MFXInitEx returns %d", sts);
        return -1;
    }

    hdl = v->opaque;
    // DPRINTF("SetHandle\n");
    sts = MFXVideoCORE_SetHandle(mfx_session, MFX_HANDLE_VA_DISPLAY, hdl->va_handle);
    if (sts != MFX_ERR_NONE) {
        error_report("MFXVideoCORE_SetHandle returns %d", sts);
        MFXClose(mfx_session);
        return -1;
    }

    /* For encoder, just query output format */
    // DPRINTF("Query output capabilities\n");
    for (out_format = VIRTIO_VIDEO_FORMAT_H264;
            out_format <= VIRTIO_VIDEO_FORMAT_H264; out_format++) {
        uint32_t w_min = 0, h_min = 0, w_max = 0, h_max = 0;
        uint32_t ctl_min = 0, ctl_max = 0, ctl = 0;
        uint32_t msdk_format = virtio_video_format_to_msdk(out_format);

        if (msdk_format == 0) continue;

        memset(&param, 0, sizeof(mfxVideoParam));
        memset(&corrected_param, 0, sizeof(mfxVideoParam));

        /* Check whether the format is supported */
        corrected_param.mfx.CodecId = msdk_format;
        sts = MFXVideoENCODE_Query(mfx_session, NULL, &corrected_param);
        if (sts != MFX_ERR_NONE && sts != MFX_WRN_PARTIAL_ACCELERATION) {
            DPRINTF("Output codec %s isn't supported by MSDK in encoder, status %d", 
                virtio_video_format_name(out_format), sts);
            virtio_video_msdk_unload_plugin(mfx_session, out_format, true);
            continue;
        }

        virtio_video_msdk_load_plugin(mfx_session, out_format, true);
        virtio_video_msdk_init_param(&param, out_format);
        /* Query the max size supported */
        w_max = VIRTIO_VIDEO_MSDK_DIMENSION_MAX;
        h_max = VIRTIO_VIDEO_MSDK_DIMENSION_MAX;

        do { /* Using mode 2 of MFXVideoENCODE_Query, it will automatically corrects the value */
            param.mfx.FrameInfo.Width = w_max;
            param.mfx.FrameInfo.Height = h_max;
            sts = MFXVideoENCODE_Query(mfx_session, &param, &param);
            if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                DPRINTF("Get the max width(%d) and height(%d)\n", w_max, h_max);
                break;
            }

            if (param.mfx.FrameInfo.Width == 0 || sts == MFX_ERR_UNSUPPORTED) 
                w_max -= VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
            if (param.mfx.FrameInfo.Height == 0 || sts == MFX_ERR_UNSUPPORTED)
                h_max -= (
                    (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ? 
                    VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS
                    );
        } while (w_max >= VIRTIO_VIDEO_MSDK_DIMENSION_MIN && 
                 h_max >= VIRTIO_VIDEO_MSDK_DIMENSION_MIN);

        /* Query the min size supported */
        w_min = VIRTIO_VIDEO_MSDK_DIMENSION_MIN;
        h_min = VIRTIO_VIDEO_MSDK_DIMENSION_MIN;

        do {
            param.mfx.FrameInfo.Width = w_min;
            param.mfx.FrameInfo.Height = h_min;
            sts = MFXVideoENCODE_Query(mfx_session, &param, &param);
            if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                DPRINTF("Get the min width(%d) and height(%d)\n", w_min, h_min);
                break;
            }
            
            if (param.mfx.FrameInfo.Width == 0 || sts == MFX_ERR_UNSUPPORTED)
                w_min += VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
            if (param.mfx.FrameInfo.Height == 0 || sts == MFX_ERR_UNSUPPORTED)
                h_min += (
                    (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ? 
                    VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS
                    );
        } while (w_min <= w_max && h_min <= h_max);

        if ((w_min > w_max) || (h_min > h_max)) { // || (fr_minD > fr_maxD)) {
            DPRINTF("failed to query frame size and frame rate for format %x in encoder\n", out_format);
            virtio_video_msdk_unload_plugin(mfx_session, out_format, false);
            continue;
        }

        // DPRINTF("Generate output format desc\n");
        /* We got the width's/height's/framerate's max and min value that MSDK supported for this format */
        out_fmt = g_new0(VirtIOVideoFormat, 1);
        virtio_video_init_format(out_fmt, out_format);
        
        out_fmt_frame = g_new0(VirtIOVideoFormatFrame, 1);
        out_fmt_frame->frame.width.min = w_min;
        out_fmt_frame->frame.width.max = w_max;
        out_fmt_frame->frame.width.step = VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
        out_fmt_frame->frame.height.min = h_min;
        out_fmt_frame->frame.height.max = h_max;
        out_fmt_frame->frame.height.step = (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
            VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS;
        out_fmt_frame->frame.num_rates = 1;
        out_fmt_frame->frame_rates = g_new0(virtio_video_format_range, 1);
        out_fmt_frame->frame_rates[0].min = 1;
        out_fmt_frame->frame_rates[0].max = 60;
        out_fmt_frame->frame_rates[0].step = VIRTIO_VIDEO_MSDK_FRAME_RATE_STEP;

        out_fmt->desc.num_frames++;
        QLIST_INSERT_HEAD(&out_fmt->frames, out_fmt_frame, next);
        QLIST_INSERT_HEAD(&v->format_list[VIRTIO_VIDEO_QUEUE_OUTPUT], out_fmt, next);
        out_fmt_nums++;

        DPRINTF("Add output caps for format %x, width [%d, %d]@%d, "
                      "height [%d, %d]@%d, rate [%d, %d]@%d  in encoder\n", out_format,
                      w_min, w_max, out_fmt_frame->frame.width.step,
                      h_min, h_max, out_fmt_frame->frame.height.step,
                      out_fmt_frame->frame_rates[0].min, out_fmt_frame->frame_rates[0].max,
                      out_fmt_frame->frame_rates[0].step);

        /* Query supported profiles */
        if (virtio_video_format_profile_range(out_format, &ctl_min, &ctl_max) < 0) {
            virtio_video_msdk_unload_plugin(mfx_session, out_format, false);
            continue;
        }

        out_fmt->profile.values = g_malloc0(sizeof(uint32_t) * (ctl_max - ctl_min) + 1);
        for (ctl = ctl_min; ctl <= ctl_max; ctl++) {
            param.mfx.CodecProfile = virtio_video_profile_to_msdk(ctl);
            if (param.mfx.CodecProfile == 0)
                continue;
            sts = MFXVideoENCODE_Query(mfx_session, &param, &corrected_param);
            if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                out_fmt->profile.values[out_fmt->profile.num++] = param.mfx.CodecProfile;
            }
        }
        if (out_fmt->profile.num == 0)
            g_free(out_fmt->profile.values);

        
        /* Query supported levels */
        if (virtio_video_format_level_range(out_format, &ctl_min, &ctl_max) < 0) {
            virtio_video_msdk_unload_plugin(mfx_session, out_format, false);
            continue;
        }
        out_fmt->level.values = g_malloc0(sizeof(uint32_t) * (ctl_max - ctl_min) + 1);
        param.mfx.CodecProfile = 0;
        for (ctl = ctl_min; ctl <= ctl_max; ctl++) {
            param.mfx.CodecLevel = virtio_video_level_to_msdk(ctl);
            if (param.mfx.CodecLevel == 0)
                continue;
            sts = MFXVideoENCODE_Query(mfx_session, &param, &corrected_param);
            if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                out_fmt->level.values[out_fmt->level.num++] = param.mfx.CodecLevel;
            }
        }
        if (out_fmt->level.num == 0)
            g_free(out_fmt->level.values);
    }

    /* For Encoding, support RGB&YUV420 for now. And frame size/rate will be specified before 
     * encode. So add two descs of them with empty frame size/rate.
     */
    // DPRINTF("Query Input capabilities\n");
    out_fmt = QLIST_FIRST(&v->format_list[VIRTIO_VIDEO_QUEUE_OUTPUT]);
    out_fmt_frame = QLIST_FIRST(&out_fmt->frames);
    for (unsigned int i = 0; i < in_fmt_nums; i++) {
        size_t len = 0;
        in_fmt = g_new0(VirtIOVideoFormat, 1);
        virtio_video_init_format(in_fmt, in_format[i]);

        in_fmt_frame = g_new0(VirtIOVideoFormatFrame, 1);
        memcpy(&in_fmt_frame->frame, &out_fmt_frame->frame, sizeof(virtio_video_format_frame));

        len = sizeof(virtio_video_format_range) * out_fmt_frame->frame.num_rates;
        in_fmt_frame->frame_rates = g_malloc0(len);
        memcpy(in_fmt_frame->frame_rates, out_fmt_frame->frame_rates, len);

        in_fmt->desc.num_frames++;
        QLIST_INSERT_HEAD(&in_fmt->frames, in_fmt_frame, next);
        QLIST_INSERT_HEAD(&v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], in_fmt, next);

    }

   QLIST_FOREACH(in_fmt, &v->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next) {
       for (unsigned int i = 0; i < out_fmt_nums; i++) {
           in_fmt->desc.mask |= BIT_ULL(i);
       }
   }
    // Close session
    MFXClose(mfx_session);

    // Init overdue stream and create overdue stream process thread
    v->overdue_run = true;
    QLIST_INIT(&v->overdue_stream_list);
    qemu_mutex_init(&v->overdue_mutex);
    qemu_event_init(&v->overdue_event, false);
    qemu_thread_create(&v->overdue_thread, "Overdue_stream_thread", virtio_video_enc_overdue_stream_thread, v, QEMU_THREAD_DETACHED);
    return 0;
}

void virtio_video_uninit_msdk_enc(VirtIOVideo *v)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
}

void virtio_video_msdk_enc_set_param_default(VirtIOVideoStream *pStream, uint32_t coded_fmt) {
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    if (pStream == NULL) return ;

    pStream->in.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    pStream->in.params.format = VIRTIO_VIDEO_FORMAT_NV12;
    pStream->in.params.min_buffers = 1;
    pStream->in.params.max_buffers = 32;
    pStream->in.params.frame_rate = 0;
    pStream->in.params.frame_width = 0;
    pStream->in.params.frame_height = 0;
    pStream->in.params.crop.left = 0;
    pStream->in.params.crop.top = 0;
    pStream->in.params.crop.width = 0;
    pStream->in.params.crop.height = 0;
    pStream->in.params.num_planes = 1;
    pStream->in.params.plane_formats[0].plane_size = 0;
    pStream->in.params.plane_formats[0].stride = 0;

    pStream->out.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    pStream->out.params.format = coded_fmt;
    pStream->out.params.min_buffers = 1;
    pStream->out.params.max_buffers = 32;
    pStream->out.params.frame_rate = 0;
    pStream->out.params.frame_width = 0;
    pStream->out.params.frame_height = 0;
    pStream->out.params.crop.left = 0;
    pStream->out.params.crop.top = 0;
    pStream->out.params.crop.width = 0;
    pStream->out.params.crop.height = 0;
    pStream->out.params.num_planes = 1;
    pStream->out.params.plane_formats[0].plane_size = 0;
    pStream->out.params.plane_formats[0].stride = 0;

    pStream->control.bitrate = 0;
    pStream->control.profile = 0;
    pStream->control.level = 0;

    switch (coded_fmt) {
    case VIRTIO_VIDEO_FORMAT_H264:
        pStream->control.profile = VIRTIO_VIDEO_PROFILE_H264_BASELINE;
        pStream->control.level = VIRTIO_VIDEO_LEVEL_H264_1_0;
        break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
        pStream->control.profile = VIRTIO_VIDEO_PROFILE_HEVC_MAIN;
        pStream->control.level = VIRTIO_VIDEO_LEVEL_HEVC_1_0;
        break;
    case VIRTIO_VIDEO_FORMAT_VP8:
        pStream->control.profile = VIRTIO_VIDEO_PROFILE_VP8_PROFILE0;
        break;
    case VIRTIO_VIDEO_FORMAT_VP9:
        pStream->control.profile = VIRTIO_VIDEO_PROFILE_VP9_PROFILE0;
        break;
    default:
        break;
    }

    pStream->state = STREAM_STATE_INIT;
}

int virtio_video_msdk_enc_stream_terminate(VirtIOVideoStream *pStream, VirtQueueElement *pElem, virtio_video_cmd_hdr *resp)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoCmd *pCmd = &pStream->inflight_cmd;
    MsdkSession *pSession = pStream->opaque;
    VirtIOVideo *v = pStream->parent;
    uint32_t len = sizeof(*resp);
    bool success = true;

    // DPRINTF("Prepare to terminate Stream %d\n", pStream->id);
    qemu_mutex_lock(&pStream->mutex);
    // DPRINTF("Lock mutex success.\n");
    qemu_mutex_lock(&pStream->mutex_out);
    // DPRINTF("Lock mutex_out success.\n");
    switch (pStream->state) {
    case STREAM_STATE_INIT :
        if (pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
            virtio_video_inflight_cmd_cancel(pStream);
        } else {
            assert(pCmd->cmd_type == 0);
        }
        break;
    case STREAM_STATE_RUNNING :
        assert(pCmd->cmd_type == 0);
        break;
    case STREAM_STATE_DRAIN :
        assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
        virtio_video_inflight_cmd_cancel(pStream);
        break;
    case STREAM_STATE_INPUT_PAUSED :
        assert((pCmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) || 
                (pCmd->cmd_type == VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL));
        virtio_video_inflight_cmd_cancel(pStream);
        break;
    case STREAM_STATE_TERMINATE :
        assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
        DPRINTF("stream %d already terminated in encoder\n", pStream->id);
        success = false;
        break;
    default :
        break;
    }

    if (success) {
        pCmd->elem = pElem;
        pCmd->cmd_type = VIRTIO_VIDEO_CMD_STREAM_DESTROY;
        pStream->state = STREAM_STATE_TERMINATE;
        pStream->bTdRun = false;
        QLIST_REMOVE(pStream, next);
        qemu_event_set(&pSession->input_notifier);
        qemu_event_set(&pSession->output_notifier);

        resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    }

    qemu_mutex_unlock(&pStream->mutex_out);
    // DPRINTF("Unlock mutex_out success.\n");
    qemu_mutex_unlock(&pStream->mutex);
    // DPRINTF("Unlock mutex success.\n");

    DPRINTF("Stream_Destroy complete.\n");

    qemu_mutex_lock(&v->overdue_mutex);
    QLIST_INSERT_HEAD(&v->overdue_stream_list, pStream, next);
    qemu_event_set(&v->overdue_event);
    qemu_mutex_unlock(&v->overdue_mutex);
    
    return len;
}

int virtio_video_msdk_init_encoder_stream(VirtIOVideoStream *pStream)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    mfxStatus sts = MFX_ERR_NONE;
    MsdkSession *pSession = NULL;
    mfxVideoParam *enc_param = NULL; // , vpp_param = {0};
    mfxFrameAllocRequest enc_req; // , vpp_req[2];// , preenc_req[2];
    VirtIOVideoFormat *pFmt = NULL;
    VirtIOVideo *pV = NULL;
    virtio_video_params *pOutputPara = NULL;
    bool bOutFmtSupported = false;
    if (pStream == NULL) {
        return -1;
    }

    if (pStream->mvp == NULL) {
        pStream->mvp = g_new(mfxVideoParam, 1);
    }
    enc_param = pStream->mvp;
    pSession = pStream->opaque;
    pV = pStream->parent;
    pOutputPara = &pStream->out.params;
    /*
     * Init encoder, query encode surface, prepare buffers
     */
    QLIST_FOREACH(pFmt, &pV->format_list[VIRTIO_VIDEO_QUEUE_OUTPUT], next) {
        DPRINTF("pFmt->desc.format = %#x, pOutputPara->format = %#x\n", pFmt->desc.format, pOutputPara->format);
        if (pFmt->desc.format == pOutputPara->format) {
            bOutFmtSupported = true;
            break;
        }
    }
    if (!bOutFmtSupported) { /* The encode format is not support yet */
        error_report("The encode format %d is not support yet!!!\n", pOutputPara->format);
        return MFX_ERR_UNSUPPORTED;
    }

    if (virtio_video_msdk_init_enc_param(enc_param, pStream) < 0) {
         return MFX_ERR_UNSUPPORTED;
    }

    DPRINTF("Set CodecProfile = %d, CodecLevel = %d\n", enc_param->mfx.CodecProfile, enc_param->mfx.CodecLevel);
    DPRINTF("Set BufferSizeInKB = %d\n", enc_param->mfx.BufferSizeInKB);
    DPRINTF("Set AysncDepth = %d\n", enc_param->AsyncDepth);
    DPRINTF("Set GopRefDist = %d\n", enc_param->mfx.GopRefDist);
    DPRINTF("Set GopPicSize = %d\n", enc_param->mfx.GopPicSize);
    DPRINTF("Set FrameRateD = %d\n", enc_param->mfx.FrameInfo.FrameRateExtD);
    DPRINTF("Set FrameRateN = %d\n", enc_param->mfx.FrameInfo.FrameRateExtN);

    sts = MFXVideoENCODE_Init(pSession->session, enc_param);
    if (sts != MFX_ERR_NONE) {
        error_report("stream %d MFXVideoENCODE_Init failed : %d  in encoder\n", pStream->id, 
                     sts);
        return MFX_ERR_UNSUPPORTED;
    }

    sts = MFXVideoENCODE_GetVideoParam(pSession->session, enc_param);
    if (sts != MFX_ERR_NONE) {
        error_report("stream %d MFXVideoENCODE_GetVideoParam failed : %d in encoder\n", pStream->id, sts);
    } else {
        pSession->bufferSizeInKB = enc_param->mfx.BufferSizeInKB;
        DPRINTF("MSDK : CodecProfile = %d, CodecLevel = %d\n", enc_param->mfx.CodecProfile, enc_param->mfx.CodecLevel);
        DPRINTF("MSDK : BufferSizeInKB = %d\n", pSession->bufferSizeInKB);
        DPRINTF("MSDK : AysncDepth = %d\n", enc_param->AsyncDepth);
        DPRINTF("MSDK : GopRefDist = %d\n", enc_param->mfx.GopRefDist);
        DPRINTF("MSDK : GopPicSize = %d\n", enc_param->mfx.GopPicSize);
        DPRINTF("MSDK : FrameRateD = %d\n", enc_param->mfx.FrameInfo.FrameRateExtD);
        DPRINTF("MSDK : FrameRateN = %d\n", enc_param->mfx.FrameInfo.FrameRateExtN);
    }

    sts = MFXVideoENCODE_QueryIOSurf(pSession->session, enc_param, &enc_req);
    if (sts != MFX_ERR_NONE) {
        error_report("stream %d MFXVideoENCODE_QueryIOSurf failed(%d)\n", pStream->id, sts);
        return MFX_ERR_UNSUPPORTED;
    }

    pSession->surface_num += enc_req.NumFrameSuggested + 2; // Use two additional surfaces for redundancy
    DPRINTF("Init surface pool, size = %d\n", pSession->surface_num);
    virtio_video_msdk_init_surface_pool(pSession, &enc_req, &(enc_param->mfx.FrameInfo), false, true);
    virtio_video_msdk_add_pf_to_pool(pSession, enc_param->mfx.BufferSizeInKB * 1024, 10); // Default pool size is 10

    // pStream->in.params.min_buffers = enc_req.NumFrameMin;
    // pStream->in.params.max_buffers = enc_req.NumFrameSuggested;
    // pStream->out.params.min_buffers = enc_req.NumFrameMin;
    // pStream->out.params.max_buffers = enc_req.NumFrameSuggested;
    // virtio_video_msdk_stream_reset_param(pStream, &enc_param, true);
    // virtio_video_msdk_stream_reset_param(pStream, &enc_param, false);

    return MFX_ERR_NONE;
}

int virtio_video_msdk_init_enc_param(mfxVideoParam *pPara, VirtIOVideoStream *pStream)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    virtio_video_params *pOutPara = NULL;
    virtio_video_params *pInPara  = NULL;
    VirtIOVideoEncodeParamPreset vvepp = {0};
    uint32_t frame_rate = 0;
    if (pPara == NULL || pStream == NULL)
        return -2;

    pOutPara = &(pStream->out.params);
    pInPara  = &(pStream->in.params);

    if (pOutPara->frame_width == 0 || pOutPara->frame_height == 0) {
        error_report("Init encoder param failed.\n");
        return -2;
    }

    // Default frame rate is 30
    frame_rate = pInPara->frame_rate == 0 ? 30 : pInPara->frame_rate;
    DPRINTF("Front-end set frame_rate = %d.\n", frame_rate);

    // H.264 encode param init
    // TODO : Add support for other formats.
    memset(pPara, 0, sizeof(mfxVideoParam));
    pPara->AsyncDepth                  = 1;
    pPara->mfx.CodecId                 = virtio_video_format_to_msdk(pOutPara->format);
    virtio_video_msdk_get_preset_param_enc(&vvepp, pPara->mfx.CodecId, frame_rate, pOutPara->frame_width, pOutPara->frame_height);
    pPara->mfx.CodecProfile            = MFX_PROFILE_AVC_BASELINE;
    pPara->mfx.CodecLevel              = MFX_LEVEL_AVC_42;

    if (pStream->control.bitrate != 0) {  // If frontend sets bitrate
        pPara->mfx.TargetKbps = pStream->control.bitrate / 1024;
    } else {
        pPara->mfx.TargetKbps = vvepp.dpp.TargetKbps;
    }

    virtio_video_msdk_convert_frame_rate(frame_rate, &(pPara->mfx.FrameInfo.FrameRateExtN), 
                    &(pPara->mfx.FrameInfo.FrameRateExtD));

    pPara->mfx.NumSlice                = 0;
    pPara->mfx.EncodedOrder            = 0;
    pPara->IOPattern                   = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    pPara->mfx.FrameInfo.FourCC        = MFX_FOURCC_NV12;
    pPara->mfx.FrameInfo.ChromaFormat  = virtio_video_msdk_fourCC_to_chroma(pPara->mfx.FrameInfo.FourCC);
    pPara->mfx.FrameInfo.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
    pPara->mfx.FrameInfo.Shift         = 0;
    pPara->mfx.FrameInfo.Width         = MSDK_ALIGN16(pOutPara->frame_width);
    pPara->mfx.FrameInfo.Height        = (MFX_PICSTRUCT_PROGRESSIVE == pPara->mfx.FrameInfo.PicStruct) ? 
        MSDK_ALIGN16(pOutPara->frame_height) : MSDK_ALIGN32(pOutPara->frame_height);
    pPara->mfx.FrameInfo.CropX         = 0;
    pPara->mfx.FrameInfo.CropY         = 0;
    pPara->mfx.FrameInfo.CropW         = pOutPara->frame_width;
    pPara->mfx.FrameInfo.CropH         = pOutPara->frame_height;
    pPara->NumExtParam                 = 0;

    return 0;
}

int virtio_video_msdk_init_vpp_param(mfxVideoParam *pEncParam, mfxVideoParam *pVppParam, 
                                         VirtIOVideoStream *pStream)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    if (pEncParam == NULL || pVppParam == NULL || pStream == NULL)
        return -1;

    uint32_t in_format = virtio_video_format_to_msdk(pStream->in.params.format);
    if (in_format == 0)
        return -1;
    
    memset(pVppParam, 0, sizeof(mfxVideoParam));
    pVppParam->vpp.Out.FourCC        = MFX_FOURCC_NV12;
    pVppParam->vpp.Out.ChromaFormat  = virtio_video_msdk_fourCC_to_chroma(pVppParam->vpp.Out.FourCC);
    pVppParam->vpp.Out.CropX         = pEncParam->mfx.FrameInfo.CropX;
    pVppParam->vpp.Out.CropY         = pEncParam->mfx.FrameInfo.CropY;
    pVppParam->vpp.Out.CropW         = pEncParam->mfx.FrameInfo.CropW;
    pVppParam->vpp.Out.CropH         = pEncParam->mfx.FrameInfo.CropH;
    pVppParam->vpp.Out.PicStruct     = pEncParam->mfx.FrameInfo.PicStruct;
    pVppParam->vpp.Out.FrameRateExtN = pEncParam->mfx.FrameInfo.FrameRateExtN;
    pVppParam->vpp.Out.FrameRateExtD = pEncParam->mfx.FrameInfo.FrameRateExtD;
    pVppParam->vpp.Out.Width         = MSDK_ALIGN16(pVppParam->vpp.Out.CropW);
    pVppParam->vpp.Out.Height        = MSDK_ALIGN16(pVppParam->vpp.Out.CropH);

    pVppParam->vpp.In                = pVppParam->vpp.Out;
    pVppParam->vpp.In.FourCC         = in_format;
    pVppParam->vpp.In.ChromaFormat   = virtio_video_msdk_fourCC_to_chroma(pVppParam->vpp.In.FourCC);

    pVppParam->IOPattern             |= MFX_IOPATTERN_IN_SYSTEM_MEMORY | 
                                        MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    
    return 0;
}

mfxStatus virtio_video_encode_submit_one_frame(VirtIOVideoStream *stream, VirtIOVideoResource *res, uint64_t timestamp, bool drain)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    MsdkSession *session = stream->opaque;
    MsdkFrame *frame = NULL;
    MsdkSurface *work_surf = NULL;
    mfxBitstream *out_bs = NULL;
    mfxStatus sts = MFX_ERR_NONE;
    VirtIOVideoFrame *pframe = NULL;
    mfxFrameSurface1 *input_surf = NULL;
    bool enc_continue = false;

    if (!drain) {
        // Pick a free MsdkSurface from SurfacePool for input
        QLIST_FOREACH(work_surf, &session->surface_pool, next) {
            if (work_surf->used == false) {
                break;
            }
        }

        if (work_surf == NULL) {
            work_surf = (MsdkSurface *)virtio_video_msdk_inc_pool_size(session, 2, false, true);
        }

        // Copy input data from resource to local surface
        DPRINTF("Copy data from resource to surface, the ts = %ld.\n", timestamp);
        virtio_video_msdk_input_surface(work_surf, res);
        work_surf->surface.Data.TimeStamp = timestamp;

        input_surf = &work_surf->surface;
    }


    do {
        // Pick a free VirtIOVideoFrame from pending_frame_pool for pending
        while (pframe == NULL) {
            QTAILQ_FOREACH(pframe, &session->pending_frame_pool, next) {
                if (pframe->used == false) {
                    frame = pframe->opaque;
                    out_bs = frame->bitstream;
                    // DPRINTF("Found pf-%d is free.\n", pframe->id);
                    break;
                }
            }

            if (pframe == NULL) {
                // If no PF is currently available, increase the pool size.
                uint32_t size = stream->mvp->mfx.BufferSizeInKB * 1024;
                virtio_video_msdk_add_pf_to_pool(session, size, 2);
            }
        }
        
        sts = MFXVideoENCODE_EncodeFrameAsync(session->session, NULL, input_surf, 
                                        out_bs, &frame->sync);
        DPRINTF("EncodeFrameAsync's return is %d.\n", sts);
        switch (sts) {
        case MFX_WRN_DEVICE_BUSY :
            usleep(1000);
            enc_continue = true;
            break;
        case MFX_ERR_NONE :
            QTAILQ_REMOVE(&session->pending_frame_pool, pframe, next);
            pframe->used = true;
            QTAILQ_INSERT_TAIL(&stream->pending_frames, pframe, next);
            qemu_event_set(&session->output_notifier);
            pframe = NULL;
            if (drain)
                enc_continue = true;
            else
                enc_continue = false;
            break;
        case MFX_ERR_MORE_DATA :
            if (drain)
                enc_continue = false;
            break;
        default :
            enc_continue = false;
            break;
        }
    } while (enc_continue);
    return sts;
}

int virtio_video_encode_retrieve_one_frame(VirtIOVideoFrame *pframe, VirtIOVideoWork *work)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *stream = work->parent;
    MsdkSession *session = stream->opaque;
    MsdkFrame *frame = NULL;
    mfxStatus sts = MFX_ERR_NONE;
    VirtIOVideoResource *res = work->resource;
    mfxBitstream *bs = NULL;
    MsdkSurface *msurface = NULL;

    if (pframe) {
        frame = pframe->opaque;
        bs    = frame->bitstream;
        do {
            sts = MFXVideoCORE_SyncOperation(session->session, frame->sync, VIRTIO_VIDEO_MSDK_TIME_TO_WAIT);
            if (sts == MFX_WRN_IN_EXECUTION && 
                    (stream->state == STREAM_STATE_TERMINATE)) {
                        virtio_video_encode_clear_work(work);
                        return -1;
                    }
        } while (sts == MFX_WRN_IN_EXECUTION);

        if (sts != MFX_ERR_NONE) {
            error_report("virtio-video-encode/%d MFXVideoCORE_SyncOperation "
                        "failed: %d", stream->id, sts); 
            virtio_video_encode_clear_work(work);
            return -1;
        }

        virtio_video_memcpy(res, 0, bs->Data, bs->DataLength);
        FILE *pTmpFile = fopen("out.h264", "ab+");
        if (pTmpFile) {
            fwrite(bs->Data, 1, bs->DataLength, pTmpFile);
            fclose(pTmpFile);
        }
        work->flags = virtio_video_get_frame_type(bs->FrameType);
        work->size = bs->DataLength;
        work->timestamp = bs->TimeStamp;

        QLIST_FOREACH(msurface, &session->surface_pool, next) {
            if (msurface->surface.Data.TimeStamp == bs->TimeStamp) {
                DPRINTF("Change surface's used to false, the ts = %lld.\n", bs->TimeStamp);
                msurface->used = false;
                break;
            }
        }

        // virtio_video_msdk_uninit_frame(pPendingFrame);
        DPRINTF("Change pf-%d's used to false.\n", pframe->id);
        // Reset pf
        pframe->used = false;
        bs->DataLength = 0;

        QTAILQ_INSERT_HEAD(&session->pending_frame_pool, pframe, next);
        DPRINTF("Send output work response(%u) timestamp(%lu), data_size(%u), frame_type(%s)\n",
                work->resource->id, work->timestamp / 1000, work->size,
                virtio_video_frame_type_name(work->flags));
    } else {
        DPRINTF("Return output resource %d without data.\n", work->resource->id);
        work->flags = 0;
        work->size = 0;
        work->timestamp = 0;
    }
    virtio_video_work_done(work);
    return 0;
}

void virtio_video_encode_clear_work(VirtIOVideoWork *work)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    if (work != NULL) {
        work->timestamp = 0;
        work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
        g_free(work->opaque);
        virtio_video_work_done(work);
    }
}

void virtio_video_encode_start_running(VirtIOVideoStream *stream)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    MsdkSession *session = NULL;
    if (stream == NULL) return ;

    session = stream->opaque;
    char thread_name[THREAD_NAME_LEN];
    snprintf(thread_name, sizeof(thread_name), "virtio-video-encode-input/%d", stream->id);
    qemu_thread_create(&session->input_thread, thread_name, virtio_video_enc_input_thread, 
                    stream, QEMU_THREAD_DETACHED);
    snprintf(thread_name, sizeof(thread_name), "virtio-video-encode-output/%d", stream->id);
    qemu_thread_create(&session->output_thread, thread_name, virtio_video_enc_output_thread, 
                    stream, QEMU_THREAD_DETACHED);
}

size_t virtio_video_msdk_enc_resource_clear(VirtIOVideoStream *stream,
    uint32_t queue_type, virtio_video_cmd_hdr *resp, VirtQueueElement *elem)
{
    VirtIOVideoCmd *cmd = &stream->inflight_cmd;
    VirtIOVideoWork *work = NULL, *tmp_work = NULL;
    // MsdkSession *session = stream->opaque;
    bool success = true;
    size_t len = sizeof(*resp);

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    switch (queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        qemu_mutex_lock(&stream->mutex);
        switch (stream->state) {
        case STREAM_STATE_INIT:
            if (cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                virtio_video_inflight_cmd_cancel(stream);
            } else {
                assert(cmd->cmd_type == 0);
            }
            success = true;
            break;
        case STREAM_STATE_RUNNING:
            assert(cmd->cmd_type == 0);
            stream->state = STREAM_STATE_INPUT_PAUSED;
            success = true;
            break;
        case STREAM_STATE_DRAIN:
            assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
            virtio_video_inflight_cmd_cancel(stream);
            stream->state = STREAM_STATE_INPUT_PAUSED;
            success = true;
            break;
        case STREAM_STATE_INPUT_PAUSED:
            assert((cmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) ||
                   (cmd->cmd_type == VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL));
            success = false;
            break;
        case STREAM_STATE_TERMINATE:
            assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            success = false;
            break;
        default:
            success = false;
            break;
        }

        if (success) {
             QTAILQ_FOREACH_SAFE(work, &stream->input_work, next, tmp_work) {
                work->timestamp = 0;
                work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                QTAILQ_REMOVE(&stream->input_work, work, next);
                g_free(work->opaque);
                virtio_video_work_done(work);
            }
            virtio_video_destroy_resource_list(stream, true);
            DPRINTF("CMD_RESOURCE_DESTROY_ALL: stream %d input resources "
                    "destroyed in encoder\n", stream->id);
            // len = 0;

            stream->state = STREAM_STATE_RUNNING;
        } else {
            DPRINTF("CMD_RESOURCE_DESTROY_ALL : stream %d currently unable to serve the request", stream->id);
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
        }
        qemu_mutex_unlock(&stream->mutex);
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        qemu_mutex_lock(&stream->mutex_out);
        if (stream->state == STREAM_STATE_TERMINATE) {
            assert(cmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            DPRINTF("CMD_RESOURCE_DESTROY_ALL: stream %d currently unable to serve the request", stream->id);
            qemu_mutex_unlock(&stream->mutex_out);
            break;
        }

        /* release the work currently being processed on decode thread */
        QTAILQ_FOREACH_SAFE(work, &stream->output_work, next, tmp_work) {
            work->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
            QTAILQ_REMOVE(&stream->output_work, work, next);
            if (work->opaque == NULL) {
                virtio_video_work_done(work);
            }
        }
        virtio_video_destroy_resource_list(stream, false);
        DPRINTF("CMD_RESOURCE_DESTROY_ALL: stream %d output resources destroyed in encoder\n", stream->id);
        stream->state = STREAM_STATE_RUNNING;
        qemu_mutex_unlock(&stream->mutex_out);
        break;
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_RESOURCE_DESTROY_ALL: invalid queue type 0x%x", queue_type);
        break;
    }

    DPRINTF("Resource_Destroy_All done.......\n");
    return len;
}
