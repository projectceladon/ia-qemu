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
void virtio_video_msdk_enc_set_param_default(VirtIOVideoStream *pStream, uint32_t coded_fmt);
int virtio_video_msdk_enc_stream_terminate(VirtIOVideoStream *pStream, VirtQueueElement *pElem);
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
    uint32_t queue_type, virtio_video_cmd_hdr *resp, VirtQueueElement *elem,
    bool destroy);

#define CHECK_AND_FILL_PARAM(A, B, C) \
if (A.C != 0) { \
    B->C = A.C; \
}



static void *virtio_video_enc_input_thread(void *arg)
{
    VirtIOVideoStream *pStream = (VirtIOVideoStream *)arg;
    VirtIOVideo *pV = pStream->parent;
    VirtIOVideoCmd *pCmd = &pStream->inflight_cmd;
    VirtIOVideoWork *pWork = NULL;
    MsdkSession *pSession = pStream->opaque;
    mfxStatus sts = MFX_ERR_NONE;
    uint32_t stream_id = pStream->id;
    bool bDrain = false;

    DPRINTF("thread virtio-video-enc-input/%d started\n", stream_id);
    object_ref(OBJECT(pV));

    while (pStream->bTdRun) {
        qemu_mutex_lock(&pStream->mutex);
        switch (pStream->state) {
        case STREAM_STATE_INIT :
            qemu_mutex_unlock(&pStream->mutex);
            qemu_event_wait(&pSession->input_notifier); // Wait first input resource comming and the input_notifier will be set
            qemu_event_reset(&pSession->input_notifier);
            if (pStream->bParamSetDone) {
                sts = virtio_video_msdk_init_encoder_stream(pStream);
                if (sts == MFX_ERR_NONE) {
                    pStream->state = STREAM_STATE_RUNNING;
                    DPRINTF("stream %d init success. Change stream state from INIT to RUNNING.\n", pStream->id);
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
                    pStream->id, 
                    virtio_video_format_name(pStream->in.params.format), virtio_video_format_name(pStream->out.params.format), 
                    pStream->in.params.frame_width,  pStream->out.params.frame_width, 
                    pStream->in.params.frame_height, pStream->out.params.frame_height, 
                    pStream->in.params.min_buffers,  pStream->out.params.min_buffers, 
                    pStream->in.params.max_buffers,  pStream->out.params.max_buffers, 
                    pStream->in.params.crop.left,    pStream->out.params.crop.left, 
                    pStream->in.params.crop.top,     pStream->out.params.crop.top, 
                    pStream->in.params.crop.width,   pStream->out.params.crop.width, 
                    pStream->in.params.crop.height,  pStream->out.params.crop.height, 
                    pStream->in.params.frame_rate,   pStream->out.params.frame_rate, 
                    pStream->in.params.num_planes,   pStream->out.params.num_planes);
                }
            }
            continue;
            break;
        case STREAM_STATE_DRAIN :
        case STREAM_STATE_RUNNING :
            if (QTAILQ_EMPTY(&pStream->input_work) && 
                pStream->state == STREAM_STATE_RUNNING && 
                pStream->queue_clear_type != VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                qemu_mutex_unlock(&pStream->mutex);
                qemu_event_wait(&pSession->input_notifier);
                qemu_event_reset(&pSession->input_notifier);
                continue;
            }

            if (pStream->queue_clear_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR);

                while (!QTAILQ_EMPTY(&pStream->input_work)) {
                    pWork = QTAILQ_FIRST(&pStream->input_work);
                    QTAILQ_REMOVE(&pStream->input_work, pWork, next);
                    virtio_video_work_done(pWork);
                }

                DPRINTF("CMD_QUEUE_CLEAR : send queue_clear_resp for intput.\n");
                virtio_video_inflight_cmd_done(pStream);
                pStream->queue_clear_type = 0;
            } else {
                pWork = QTAILQ_FIRST(&pStream->input_work);
                bDrain = pStream->state == STREAM_STATE_DRAIN && pWork == QTAILQ_LAST(&pStream->input_work);
                // DPRINTF("Prepare to process input resource : %d\n", pWork->resource->id);
                sts = virtio_video_encode_submit_one_frame(pStream, pWork->resource, pWork->timestamp, bDrain);
                QTAILQ_REMOVE(&pStream->input_work, pWork, next);
                if (sts != MFX_ERR_NONE && sts != MFX_ERR_MORE_DATA) {
                    pWork->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                }

                if (bDrain) {
                    pWork->flags = MFX_BITSTREAM_EOS;
                }
                pWork->timestamp = 0;
                virtio_video_work_done(pWork);

                sts = MFX_ERR_NONE;
                while (bDrain && sts != MFX_ERR_MORE_DATA) {
                    sts = virtio_video_encode_submit_one_frame(NULL, NULL, 0, true);
                }
            }
            break;
        case STREAM_STATE_INPUT_PAUSED :
            break;
        case STREAM_STATE_TERMINATE :
            break;
        default :
            break;
        }
        qemu_mutex_unlock(&pStream->mutex);
    }

    DPRINTF("thread virtio-video-enc-input/%d exited\n", stream_id);
    return NULL;
}

static void *virtio_video_enc_output_thread(void *arg)
{
    VirtIOVideoStream *pStream = (VirtIOVideoStream *)arg;
    VirtIOVideo *pV = pStream->parent;
    VirtIOVideoCmd *pCmd = &pStream->inflight_cmd;
    VirtIOVideoWork *pOutWork = NULL;
    MsdkSession *pSession = pStream->opaque;
    VirtIOVideoFrame *pPendingFrame = NULL;
    uint32_t stream_id = pStream->id;
    // FILE *pTmpFile = fopen("test_output.h264", "wb+");
    // mfxBitstream *pTmpBs = NULL;

    DPRINTF("thread virtio-video-enc-output/%d started\n", stream_id);
    object_ref(OBJECT(pV));

    while (pStream->bTdRun) {
        qemu_mutex_lock(&pStream->mutex_out);
        switch (pStream->state) {
            case STREAM_STATE_INIT :
                qemu_mutex_unlock(&pStream->mutex_out);
                qemu_event_wait(&pSession->output_notifier);
                qemu_event_reset(&pSession->output_notifier);
                continue;
                break;
            case STREAM_STATE_RUNNING :
                if ((QTAILQ_EMPTY(&pStream->output_work) || QTAILQ_EMPTY(&pStream->pending_frames)) && 
                    pStream->queue_clear_type != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                    qemu_mutex_unlock(&pStream->mutex_out);
                    qemu_event_wait(&pSession->output_notifier);
                    qemu_event_reset(&pSession->output_notifier);
                    continue;
                }

                if (pStream->queue_clear_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                    assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR);

                    while (!QTAILQ_EMPTY(&pStream->output_work)) {
                        pOutWork = QTAILQ_FIRST(&pStream->output_work);
                        QTAILQ_REMOVE(&pStream->output_work, pOutWork, next);
                        virtio_video_work_done(pOutWork);
                    }
                    DPRINTF("CMD_QUEUE_CLEAR : send queue_clear_resp for output.\n");
                    virtio_video_inflight_cmd_done(pStream);
                    pStream->queue_clear_type = 0;
                } else {
                    assert(pCmd->cmd_type == 0);
                    pPendingFrame = QTAILQ_FIRST(&pStream->pending_frames);
                    pOutWork = QTAILQ_FIRST(&pStream->output_work);
                    QTAILQ_REMOVE(&pStream->pending_frames, pPendingFrame, next);
                    QTAILQ_REMOVE(&pStream->output_work, pOutWork, next);

                    virtio_video_encode_retrieve_one_frame(pPendingFrame, pOutWork);
                }
                break;
            case STREAM_STATE_DRAIN :
                break;
            case STREAM_STATE_TERMINATE :
                break;
            default :
                break;
        }
        qemu_mutex_unlock(&pStream->mutex_out);
    }
    DPRINTF("thread virtio-video-enc-output/%d exited\n", stream_id);
    return NULL;
}


size_t virtio_video_msdk_enc_stream_create(VirtIOVideo *v,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoFormat *pFmt = NULL;
    VirtIOVideoStream *pStream = NULL;
    MsdkHandle *pHdl = v->opaque;
    MsdkSession *pSession = NULL;
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
    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == resp->stream_id) {
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
    QLIST_FOREACH(pFmt, &v->format_list[VIRTIO_VIDEO_QUEUE_OUTPUT], next) {
        if (pFmt->desc.format == req->coded_format) {
            break;
        }
    }

    if (pFmt == NULL) {
        error_report("CMD_STREAM_CREATE: unsupported codec format %s in encoder", 
            virtio_video_format_name(req->coded_format));
        return len;
    }

    pSession = g_new0(MsdkSession, 1);
    sts = MFXInitEx(param, &pSession->session);
    if (sts != MFX_ERR_NONE) {
        error_report("CMD_STREAM_CREATE: MFXInitEx failed: %d in encoder", sts);
        g_free(pSession);
        return len;
    }

    sts = MFXVideoCORE_SetHandle(pSession->session, MFX_HANDLE_VA_DISPLAY, pHdl->va_handle);
    if (sts != MFX_ERR_NONE) {
        error_report("CMD_STREAM_CREATE: MFXVideoCORE_SetHandle failed: %d", sts);
        MFXClose(pSession->session);
        g_free(pSession);
        return len;
    }

    pStream = g_new0(VirtIOVideoStream, 1);
    pStream->opaque = pSession;
    pStream->parent = v;

    pStream->id = req->hdr.stream_id;
    pStream->in.mem_type = req->in_mem_type;
    pStream->out.mem_type = req->out_mem_type;
    pStream->in.setted = false;
    pStream->out.setted = false;
    memcpy(pStream->tag, req->tag, strlen((char *)req->tag));

    virtio_video_msdk_enc_set_param_default(pStream, req->coded_format);

    for (i = 0; i < VIRTIO_VIDEO_QUEUE_NUM; i++) {
        QLIST_INIT(&pStream->resource_list[i]);
    }
    QTAILQ_INIT(&pStream->pending_frames);
    QTAILQ_INIT(&pStream->input_work);
    QTAILQ_INIT(&pStream->output_work);
    qemu_mutex_init(&pStream->mutex);
    qemu_mutex_init(&pStream->mutex_out);
    qemu_event_init(&pSession->input_notifier, false);
    qemu_event_init(&pSession->output_notifier, false);
    QLIST_INIT(&pSession->surface_pool);
    QLIST_INIT(&pSession->vpp_surface_pool);
    
    QLIST_INSERT_HEAD(&v->stream_list, pStream, next);

    pStream->bTdRun = true;
    pStream->bParamSetDone = false;
    pStream->state = STREAM_STATE_INIT;
    pStream->queue_clear_type = 0;
    pStream->mvp = NULL;

    virtio_video_encode_start_running(pStream);

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;

    DPRINTF("CMD_STREAM_CREATE: stream %d [%s] format %s created in encoder\n", 
            pStream->id, pStream->tag, virtio_video_format_name(req->coded_format));
    
    return len;
}

size_t virtio_video_msdk_enc_stream_destroy(VirtIOVideo* v,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp, 
    VirtQueueElement *elem)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == req->hdr.stream_id) {
            break;
        }
    }

    if (pStream == NULL) {
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        return len;
    }

    return virtio_video_msdk_enc_stream_terminate(pStream, elem);
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
        error_report("CMD_RESOURCE_QUEUE: stream %d not found.\n", pStream->id);
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
                                                resp, elem, true);
}

size_t virtio_video_msdk_enc_queue_clear(VirtIOVideo *v,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp, 
    VirtQueueElement *elem)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = NULL;
    MsdkSession *pSession = NULL;
    VirtIOVideoCmd *pCmd = NULL;
    size_t len = sizeof(*resp);
    QemuMutex *pMtx = NULL;
    QemuEvent *pEvt = NULL;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == req->hdr.stream_id) {
            pCmd = &pStream->inflight_cmd;
            pSession = pStream->opaque;
            break;
        }
    }

    if (pStream == NULL) {
        error_report("CMD_QUEUE_CLEAR : stream %d not found.\n", req->hdr.stream_id);
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        return len;
    }

    if (pCmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) {
        error_report("CMD_QUEUE_CLEAR : stream %d is already processing queue_clear.\n", pStream->id);
        return len;
    }

    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT :
        pMtx = &pStream->mutex;
        pEvt = &pSession->input_notifier;
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT :
        pMtx = &pStream->mutex_out;
        pEvt = &pSession->output_notifier;
        break;
    }

    if (pMtx && pEvt) {
        qemu_mutex_lock(pMtx);
        pCmd->cmd_type = VIRTIO_VIDEO_CMD_QUEUE_CLEAR;
        pStream->queue_clear_type = req->queue_type;
        pCmd->elem = elem;
        qemu_event_set(pEvt);
        qemu_mutex_unlock(pMtx);
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
        error_report("CMD_GET_PARAMS : stream %d not found in encoder\n", pStream->id);
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

int virtio_video_msdk_enc_stream_terminate(VirtIOVideoStream *pStream, VirtQueueElement *pElem)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoCmd *pCmd = &pStream->inflight_cmd;
    MsdkSession *pSession = pStream->opaque;

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
        qemu_mutex_unlock(&pStream->mutex_out);
        qemu_mutex_unlock(&pStream->mutex);
        return sizeof(virtio_video_cmd_hdr);
        break;
    default :
        break;
    }

    pCmd->elem = pElem;
    pCmd->cmd_type = VIRTIO_VIDEO_CMD_STREAM_DESTROY;
    pStream->state = STREAM_STATE_TERMINATE;
    pStream->bTdRun = false;
    QLIST_REMOVE(pStream, next);
    qemu_event_set(&pSession->input_notifier);
    qemu_event_set(&pSession->output_notifier);

    qemu_mutex_unlock(&pStream->mutex_out);
    // DPRINTF("Unlock mutex_out success.\n");
    qemu_mutex_unlock(&pStream->mutex);
    // DPRINTF("Unlock mutex success.\n");

    DPRINTF("Stream_Destroy complete.\n");
    // Waiting for input and output thread terminate
    // DPRINTF("Waiting for input and output thread terminate.\n");
    // qemu_thread_join(&pSession->input_thread);
    // qemu_thread_join(&pSession->output_thread);

    // TODO : Free the all alloc buffers
    // g_free(pSession);
    // g_free(pStream);
    return 0;
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
    // mfxExtCodingOption2 *pOpt2 = NULL;
    uint32_t frame_rate = 0;
    // VirtIOVideoEncodeParamPreset ParamPreset = {0};
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
    pPara->mfx.TargetUsage             = vvepp.epp.TargetUsage;
    pPara->mfx.RateControlMethod       = vvepp.epp.RateControlMethod; // Default is CBR(constant bit rate)
    pPara->mfx.GopRefDist              = vvepp.epp.GopRefDist;
    pPara->mfx.GopPicSize              = vvepp.dpp.GopPicSize;
    pPara->mfx.NumRefFrame             = 0;
    pPara->mfx.IdrInterval             = 0;
    pPara->mfx.CodecProfile            = 0;
    pPara->mfx.CodecLevel              = 0;
    pPara->mfx.MaxKbps                 = vvepp.dpp.MaxKbps;
    pPara->mfx.InitialDelayInKB        = 0;
    pPara->mfx.GopOptFlag              = 0;
    pPara->mfx.BufferSizeInKB          = vvepp.dpp.TargetKbps / 8;
    
    // pPara->mfx.CodecProfile            = virtio_video_profile_to_msdk(pStream->control.profile);
    // pPara->mfx.CodecLevel              = virtio_video_level_to_msdk(pStream->control.level);

    if (pPara->mfx.RateControlMethod == MFX_RATECONTROL_CQP) {
        pPara->mfx.QPI                 = 0;
        pPara->mfx.QPB                 = 0;
        pPara->mfx.QPP                 = 0;
    } else if (pPara->mfx.RateControlMethod == MFX_RATECONTROL_ICQ || 
        pPara->mfx.RateControlMethod == MFX_RATECONTROL_LA_ICQ) {
        pPara->mfx.ICQQuality          = 0;
    } else if (pPara->mfx.RateControlMethod == MFX_RATECONTROL_AVBR) {

    } else {
        pPara->mfx.TargetKbps = vvepp.dpp.TargetKbps; // Default bitrate
    }

    if (pStream->control.bitrate != 0) {  // If frontend sets bitrate
        pPara->mfx.TargetKbps = pStream->control.bitrate / 1024;
    }

    pPara->mfx.NumSlice                = 0;

    virtio_video_msdk_convert_frame_rate(frame_rate, &(pPara->mfx.FrameInfo.FrameRateExtN), 
                    &(pPara->mfx.FrameInfo.FrameRateExtD));

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

    // if (pPara->mfx.CodecId == MFX_CODEC_AVC) {
    //     pPara->ExtParam = g_malloc0(sizeof(mfxExtBuffer *));
    //     pOpt2 = g_new0(mfxExtCodingOption2, 1);
    //     pOpt2->LookAheadDepth          = 0;
    //     pOpt2->MaxSliceSize            = 0;
    //     pOpt2->MaxFrameSize            = 0;
    //     pOpt2->BRefType                = vvepp.epp.BRefType;
    //     pOpt2->BitrateLimit            = MFX_CODINGOPTION_OFF;
    //     pOpt2->ExtBRC                  = 0;
    //     pOpt2->IntRefType              = vvepp.epp.IntRefType;
    //     pOpt2->IntRefCycleSize         = vvepp.epp.IntRefCycleSize;
    //     pOpt2->IntRefQPDelta           = vvepp.epp.IntRefQPDelta;
    //     pOpt2->AdaptiveI               = 0;
    //     pOpt2->AdaptiveB               = 0;
    //     pOpt2->Header.BufferId         = MFX_EXTBUFF_CODING_OPTION2;
    //     pOpt2->Header.BufferSz         = sizeof(*pOpt2);
    //     pPara->ExtParam[0]             = (mfxExtBuffer *)pOpt2;
    //     pPara->NumExtParam             += 1;
    // }

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

mfxStatus virtio_video_encode_submit_one_frame(VirtIOVideoStream *pStream, VirtIOVideoResource *pRes, uint64_t timestamp, bool bDrain)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    MsdkSession *pSession = pStream->opaque;
    MsdkFrame *pFrame = NULL;
    MsdkSurface *pWorkSurf = NULL;
    mfxBitstream *pOutBs = NULL;
    mfxStatus sts = MFX_ERR_NONE;
    VirtIOVideoFrame *pPendingFrame = NULL;
    mfxFrameSurface1 *pInputSurface = NULL;

    if (!bDrain) {
        // Pick a free MsdkSurface from SurfacePool for input
        QLIST_FOREACH(pWorkSurf, &pSession->surface_pool, next) {
            if (pWorkSurf->used == false) {
                break;
            }
        }

        if (pWorkSurf == NULL) {
            pWorkSurf = (MsdkSurface *)virtio_video_msdk_inc_pool_size(pSession, 2, false, true);
        }

        // Copy input data from resource to local surface
        DPRINTF("Copy data from resource to surface, the ts = %ld.\n", timestamp);
        virtio_video_msdk_input_surface(pWorkSurf, pRes);
        pWorkSurf->surface.Data.TimeStamp = timestamp;
    }

    // Pick a free VirtIOVideoFrame from pending_frames for pending
    while (pPendingFrame == NULL) {
        QTAILQ_FOREACH(pPendingFrame, &pSession->pending_frame_pool, next) {
            if (pPendingFrame->used == false) {
                pFrame = pPendingFrame->opaque;
                pOutBs = pFrame->pBitStream;
                DPRINTF("Found pf-%d is free.\n", pPendingFrame->id);
                break;
            }
        }

        if (pPendingFrame == NULL) {
            // If no PF is currently available, increase the pool size.
            uint32_t size = pStream->mvp->mfx.BufferSizeInKB * 1024;
            virtio_video_msdk_add_pf_to_pool(pSession, size, 2);
        }
    }
    if (pWorkSurf != NULL) {
        pInputSurface = &pWorkSurf->surface;
    }

    do {
        
        sts = MFXVideoENCODE_EncodeFrameAsync(pSession->session, NULL, pInputSurface, 
                                          pOutBs, &pFrame->sync);

        DPRINTF("EncodeFrameAsync's return is %d.\n", sts);
        switch (sts) {
        case MFX_WRN_DEVICE_BUSY :
            usleep(1000);
            break;
        case MFX_ERR_NONE :
            break;
        default :
            break;
        }
    } while (sts == MFX_WRN_DEVICE_BUSY);

    if (sts == MFX_ERR_NONE) {
        DPRINTF("Change pf-%d's used to true.\n", pPendingFrame->id);
        pPendingFrame->used = true;
        QTAILQ_INSERT_TAIL(&pStream->pending_frames, pPendingFrame, next);
        qemu_event_set(&pSession->output_notifier);
    }

    return sts;
}

int virtio_video_encode_retrieve_one_frame(VirtIOVideoFrame *pPendingFrame, VirtIOVideoWork *pOutWork)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = pOutWork->parent;
    MsdkSession *pSession = pStream->opaque;
    MsdkFrame *pFrame = pPendingFrame->opaque;
    mfxStatus sts = MFX_ERR_NONE;
    VirtIOVideoResource *pRes = pOutWork->resource;
    mfxBitstream *pBs = pFrame->pBitStream;
    MsdkSurface *pSurface = NULL;
    FILE *pTmpFile = fopen("out.h264", "ab+");

    do {
        sts = MFXVideoCORE_SyncOperation(pSession->session, pFrame->sync, VIRTIO_VIDEO_MSDK_TIME_TO_WAIT);
        if (sts == MFX_WRN_IN_EXECUTION && 
                (pStream->state == STREAM_STATE_TERMINATE)) {
                     virtio_video_encode_clear_work(pOutWork);
                     return -1;
                 }
    } while (sts == MFX_WRN_IN_EXECUTION);

    if (sts != MFX_ERR_NONE) {
        error_report("virtio-video-encode/%d MFXVideoCORE_SyncOperation "
                      "failed: %d", pStream->id, sts); 
        virtio_video_encode_clear_work(pOutWork);
        return -1;
    }

    virtio_video_memcpy(pRes, 0, pBs->Data, pBs->DataLength);
    if (pTmpFile) {
        fwrite(pBs->Data, 1, pBs->DataLength, pTmpFile);
        fclose(pTmpFile);
    }
    pOutWork->flags = virtio_video_get_frame_type(pBs->FrameType);
    pOutWork->size = pBs->DataLength;
    pOutWork->timestamp = pBs->TimeStamp;

    QLIST_FOREACH(pSurface, &pSession->surface_pool, next) {
        if (pSurface->surface.Data.TimeStamp == pBs->TimeStamp) {
            DPRINTF("Change surface's used to false, the ts = %lld.\n", pBs->TimeStamp);
            pSurface->used = false;
            break;
        }
    }

    // virtio_video_msdk_uninit_frame(pPendingFrame);
    DPRINTF("Change pf-%d's used to false.\n", pPendingFrame->id);
    // Reset pf
    pPendingFrame->used = false;
    pBs->DataLength = 0;
    DPRINTF("Send output work response(%u) timestamp(%lu), data_size(%u), frame_type(%s)\n",
            pOutWork->resource->id, pOutWork->timestamp / 1000, pOutWork->size,
            virtio_video_frame_type_name(pOutWork->flags));
    virtio_video_work_done(pOutWork);
    return 0;
}

void virtio_video_encode_clear_work(VirtIOVideoWork *pWork)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    if (pWork != NULL) {
        pWork->timestamp = 0;
        pWork->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
        g_free(pWork->opaque);
        virtio_video_work_done(pWork);
    }
}

void virtio_video_encode_start_running(VirtIOVideoStream *pStream)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    MsdkSession *pSession = NULL;
    if (pStream == NULL) return ;

    pSession = pStream->opaque;
    char thread_name[THREAD_NAME_LEN];
    snprintf(thread_name, sizeof(thread_name), "virtio-video-encode-input/%d", pStream->id);
    qemu_thread_create(&pSession->input_thread, thread_name, virtio_video_enc_input_thread, 
                    pStream, QEMU_THREAD_DETACHED);
    snprintf(thread_name, sizeof(thread_name), "virtio-video-encode-output/%d", pStream->id);
    qemu_thread_create(&pSession->output_thread, thread_name, virtio_video_enc_output_thread, 
                    pStream, QEMU_THREAD_DETACHED);
}

size_t virtio_video_msdk_enc_resource_clear(VirtIOVideoStream *pStream,
    uint32_t queue_type, virtio_video_cmd_hdr *resp, VirtQueueElement *elem,
    bool destroy)
{
    VirtIOVideoCmd *pCmd = &pStream->inflight_cmd;
    VirtIOVideoWork *pWork = NULL, *pTmpWork = NULL;
    MsdkSession *pSession = pStream->opaque;
    bool bSuccess = true;
    size_t len = sizeof(*resp);

    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
    switch (queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        qemu_mutex_lock(&pStream->mutex);
        switch (pStream->state) {
        case STREAM_STATE_INIT:
            if (pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN) {
                virtio_video_inflight_cmd_cancel(pStream);
            } else {
                assert(pCmd->cmd_type == 0);
            }

            QTAILQ_FOREACH_SAFE(pWork, &pStream->input_work, next, pTmpWork) {
                pWork->timestamp = 0;
                pWork->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
                QTAILQ_REMOVE(&pStream->input_work, pWork, next);
                g_free(pWork->opaque);
                virtio_video_work_done(pWork);
            }
            if (destroy) {
                virtio_video_destroy_resource_list(pStream, true);
                DPRINTF("CMD_RESOURCE_DESTROY_ALL: stream %d input resources "
                        "destroyed in encoder\n", pStream->id);
            } else {
                DPRINTF("CMD_QUEUE_CLEAR: stream %d input queue cleared in encoder\n",
                        pStream->id);
            }
            break;
        case STREAM_STATE_RUNNING:
            assert(pCmd->cmd_type == 0);
            pStream->state = STREAM_STATE_INPUT_PAUSED;
            break;
        case STREAM_STATE_DRAIN:
            assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
            virtio_video_inflight_cmd_cancel(pStream);
            pStream->state = STREAM_STATE_INPUT_PAUSED;
            break;
        case STREAM_STATE_INPUT_PAUSED:
            assert((pCmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) ||
                   (pCmd->cmd_type == VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL));
            if (destroy && pCmd->cmd_type == VIRTIO_VIDEO_CMD_QUEUE_CLEAR) {
                virtio_video_inflight_cmd_cancel(pStream);
            } else {
                bSuccess = false;
            }
            break;
        case STREAM_STATE_TERMINATE:
            assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            bSuccess = false;
            break;
        default:
            bSuccess = false;
            break;
        }

        if (bSuccess) {
            pCmd->elem = elem;
            pCmd->cmd_type = destroy ? VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL :
                                      VIRTIO_VIDEO_CMD_QUEUE_CLEAR;
            qemu_event_set(&pSession->output_notifier);

            if (destroy) {
                DPRINTF("CMD_RESOURCE_DESTROY_ALL (async): stream %d start "
                        "to destroy input resources\n", pStream->id);
            } else {
                DPRINTF("CMD_QUEUE_CLEAR (async): stream %d start "
                        "to clear input queue\n", pStream->id);
            }
            len = 0;
        } else {
            DPRINTF("%s: stream %d currently unable to serve the request",
                    destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                    pStream->id);
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
        }
        qemu_mutex_unlock(&pStream->mutex);
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        qemu_mutex_lock(&pStream->mutex_out);
        if (pStream->state == STREAM_STATE_TERMINATE) {
            assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            DPRINTF("%s: stream %d currently unable to serve the request",
                    destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                    pStream->id);
            qemu_mutex_unlock(&pStream->mutex_out);
            break;
        }

        /* release the work currently being processed on decode thread */
        QTAILQ_FOREACH_SAFE(pWork, &pStream->output_work, next, pTmpWork) {
            pWork->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
            QTAILQ_REMOVE(&pStream->output_work, pWork, next);
            if (pWork->opaque == NULL) {
                virtio_video_work_done(pWork);
            }
        }
        if (destroy) {
            virtio_video_destroy_resource_list(pStream, false);
            DPRINTF("CMD_RESOURCE_DESTROY_ALL: stream %d output resources "
                    "destroyed\n", pStream->id);
        } else {
            DPRINTF("CMD_QUEUE_CLEAR: stream %d output queue cleared\n",
                    pStream->id);
        }
        qemu_mutex_unlock(&pStream->mutex_out);
        break;
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("%s: invalid queue type 0x%x",
                     destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                     queue_type);
        break;
    }

    DPRINTF("Resource_Destroy_All done.......\n");
    return len;
}