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

#define THREAD_NAME_LEN 48
void virtio_video_msdk_enc_set_param_default(VirtIOVideoStream *pStream, uint32_t coded_fmt);
int virtio_video_msdk_enc_stream_terminate(VirtIOVideoStream *pStream, VirtQueueElement *pElem);
int virtio_video_msdk_init_encoder_stream(VirtIOVideoStream *pStream);
int virtio_video_msdk_init_enc_param(mfxVideoParam *pPara, VirtIOVideoStream *pStream);
int virtio_video_msdk_init_vpp_param(mfxVideoParam *pEncParam, mfxVideoParam *pVppParam, 
                                         VirtIOVideoStream *pStream);
mfxStatus virtio_video_encode_submit_one_frame(VirtIOVideoWork *pWork);
int virtio_video_encode_fill_input_data(MsdkFrame *pFrame, VirtIOVideoResource *pRes, 
                                        uint32_t format);
int virtio_video_encode_retrieve_one_frame(VirtIOVideoWork *pWorkOut);
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
    VirtIOVideoCmd *pCmd = NULL;
    VirtIOVideoWork *pWork = NULL;
    VirtIOVideoWork *pOutWork = NULL;
    MsdkFrame *pFrame = NULL;
    MsdkFrame *pOutFrame = NULL;
    MsdkSession *pSession = pStream->opaque;
    mfxStatus sts = MFX_ERR_NONE;
    uint32_t stream_id = pStream->id;

    DPRINTF("thread virtio-video-input/%d started\n", stream_id);
    object_ref(OBJECT(pV));

    while (pStream->bTdRun) {
        qemu_mutex_lock(&pStream->mutex);
        switch (pStream->state) {
        case STREAM_STATE_INIT :
            break;
        case STREAM_STATE_RUNNING :
            if (QTAILQ_EMPTY(&pStream->input_work) || 
                QTAILQ_EMPTY(&pStream->output_work)) {
                qemu_mutex_unlock(&pStream->mutex);
                qemu_event_wait(&pSession->input_notifier);
                qemu_event_reset(&pSession->input_notifier);
                continue;
            }

            pCmd = &pStream->inflight_cmd;
            assert(pCmd->cmd_type == 0);

            pWork = QTAILQ_FIRST(&pStream->input_work);
            pOutWork = QTAILQ_FIRST(&pStream->output_work);
            QTAILQ_REMOVE(&pStream->input_work, pWork, next);
            QTAILQ_REMOVE(&pStream->output_work, pOutWork, next);
            qemu_mutex_unlock(&pStream->mutex);

            pFrame = pWork->opaque;
            pOutFrame = pOutWork->opaque;
            pFrame->pBitStream = pOutFrame->pBitStream;
            sts = virtio_video_encode_submit_one_frame(pWork);
            if (sts != MFX_ERR_NONE) {
                virtio_video_encode_clear_work(pWork);
            }

            qemu_mutex_lock(&pStream->mutex_enc);
            QTAILQ_INSERT_TAIL(&pStream->pending_work, pOutWork, next);
            qemu_event_set(&pSession->output_notifier);
            qemu_mutex_unlock(&pStream->mutex_enc);
            continue;
            break;
        case STREAM_STATE_DRAIN :
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


    return NULL;
}

static void *virtio_video_enc_output_thread(void *arg)
{
    VirtIOVideoStream *pStream = (VirtIOVideoStream *)arg;
    VirtIOVideo *pV = pStream->parent;
    VirtIOVideoCmd *pCmd = &pStream->inflight_cmd;
    VirtIOVideoWork *pWorkOut = NULL;
    MsdkSession *pSession = pStream->opaque;
    uint32_t stream_id = pStream->id;

    DPRINTF("thread virtio-video-output/%d started\n", stream_id);
    object_ref(OBJECT(pV));

    while (pStream->bTdRun) {
        qemu_mutex_lock(&pStream->mutex_enc);
        switch (pStream->state) {
            case STREAM_STATE_INIT :
                break;
            case STREAM_STATE_RUNNING :
                if (QTAILQ_EMPTY(&pStream->pending_work)) {
                    qemu_mutex_unlock(&pStream->mutex_enc);
                    qemu_event_wait(&pSession->output_notifier);
                    qemu_event_reset(&pSession->output_notifier);
                    continue;
                }

                assert(pCmd->cmd_type == 0);
                pWorkOut = QTAILQ_FIRST(&pStream->pending_work);
                QTAILQ_REMOVE(&pStream->pending_work, pWorkOut, next);
                qemu_mutex_unlock(&pStream->mutex_enc);

                virtio_video_encode_retrieve_one_frame(pWorkOut);
                continue;
                break;
            case STREAM_STATE_DRAIN :
                break;
            case STREAM_STATE_TERMINATE :
                break;
            default :
                break;
        }
        qemu_mutex_unlock(&pStream->mutex_enc);
    }
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
    QTAILQ_INIT(&pStream->pending_work);
    QTAILQ_INIT(&pStream->input_work);
    QTAILQ_INIT(&pStream->output_work);
    qemu_mutex_init(&pStream->mutex);
    qemu_mutex_init(&pStream->mutex_enc);
    qemu_event_init(&pSession->input_notifier, false);
    qemu_event_init(&pSession->output_notifier, false);
    QLIST_INIT(&pSession->surface_pool);
    QLIST_INIT(&pSession->vpp_surface_pool);
    
    QLIST_INSERT_HEAD(&v->stream_list, pStream, next);

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
    return 0;
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
    MsdkFrame *pFrame = NULL;
    mfxBitstream *pBs = NULL;
    VirtIOVideoCmd *pCmd = NULL;
    VirtIOVideoResource *pRes = NULL;
    VirtIOVideoWork *pWork = NULL;
    size_t len = 0;
    uint8_t queue_type = VIRTIO_VIDEO_QUEUE_INPUT;
    bool bQueued = false;
    bool bQueueSuccess = false;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH(pStream, &v->stream_list, next) {
        if (pStream->id == req->hdr.stream_id) {
            pSession = pStream->opaque;
            pCmd = &pStream->inflight_cmd;
            break;
        }
    }

    if (pStream == NULL) {
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        error_report("%s %s: CMD_RESOURCE_QUEUE: stream %d not found.\n", __FILE__, __FUNCTION__, 
                     pStream->id);
        return len;
    }

    // Check whether the encoder has been initialized
    if (pStream->state == STREAM_STATE_INIT) {
        error_report("%s %s : CMD_RESOURCE_QUEUE : stream %d has not been init yet\n", __FILE__, __FUNCTION__, 
                     pStream->id);
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
        error_report("%s %s : stream %d resource %d for %d(0 - input 1 - output) not found\n", 
                     __FILE__, __FUNCTION__, pStream->id, req->resource_id, queue_type);
        return len;
    }
    switch (req->queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT :
        if (!virtio_video_format_is_valid(pStream->in.params.format, req->num_data_sizes)) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
            error_report("%s %s : CMD_RESOURCE_QUEUE: stream %d try to queue "
                    "a resource with num_data_sizes=%d for input queue "
                    "whose format is %s", __FILE__, __FUNCTION__, 
                    pStream->id, req->num_data_sizes,
                    virtio_video_format_name(pStream->in.params.format));
            break;
        }

        qemu_mutex_lock(&pStream->mutex);

        // Check whether the resource is already queued
        QTAILQ_FOREACH(pWork, &pStream->pending_work, next) {
            if (pRes->id == pWork->resource->id) {
                bQueued = true;
                break;
            }
        }
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

        pFrame = g_new0(MsdkFrame, 1);
        virtio_video_encode_fill_input_data(pFrame, pRes, pStream->in.params.format);
        
        
        pWork = g_new0(VirtIOVideoWork, 1);
        pWork->parent = pStream;
        pWork->elem = elem;
        pWork->resource = pRes;
        pWork->queue_type = req->queue_type;
        pWork->timestamp = req->timestamp;
        pWork->opaque = pFrame;
        

        switch (pStream->state) {
        case STREAM_STATE_RUNNING : // 
            assert(pCmd->cmd_type == 0);
            QTAILQ_INSERT_TAIL(&pStream->input_work, pWork, next);
            qemu_event_set(&pSession->input_notifier);
            bQueueSuccess = true;
            break;
        case STREAM_STATE_DRAIN:
            assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DRAIN);
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
            g_free(pWork->opaque);
            g_free(pWork);
            DPRINTF("CMD_RESOURCE_QUEUE: stream %d currently unable to queue input resources\n", pStream->id);
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
            break;
        }

        qemu_mutex_lock(&pStream->mutex_enc);
        QTAILQ_FOREACH(pWork, &pStream->output_work, next) {
            if (pRes->id == pWork->resource->id) {
                error_report("CMD_RESOURCE_QUEUE: stream %d output resource %d "
                             "already queued, cannot be queued again", pStream->id, pRes->id);
                qemu_mutex_unlock(&pStream->mutex_enc);
                return len;
            }
        }
        if (pStream->state == STREAM_STATE_TERMINATE) {
            assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            qemu_mutex_unlock(&pStream->mutex_enc);
            DPRINTF("CMD_RESOURCE_QUEUE: stream %d currently unable to "
                    "queue output resources\n", pStream->id);
            return len;
        }

        pFrame = g_new0(MsdkFrame, 1);
        pBs = g_new0(mfxBitstream, 1);
        pBs->Data = pRes->slices[0]->page.base;
        pBs->DataLength = pRes->slices[0]->page.len;
        pBs->MaxLength = pBs->DataLength;
        pFrame->pBitStream = pBs;

        pWork = g_new0(VirtIOVideoWork, 1);
        pWork->parent = pStream;
        pWork->elem = elem;
        pWork->resource = pRes;
        pWork->queue_type = req->queue_type;
        pWork->opaque = pFrame;

        QTAILQ_INSERT_TAIL(&pStream->output_work, pWork, next);
        qemu_event_set(&pSession->input_notifier);
        qemu_mutex_unlock(&pStream->mutex_enc);

        DPRINTF("CMD_RESOURCE_QUEUE : stream %d queued output resource %d\n", pStream->id, pRes->id);
        len = 0;
        break;
    default :
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_RESOURCE_QUEUE : invalid queue type 0x%x\n", req->queue_type);
    break;
    }

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
        DPRINTF("CMD_GET_PARAMS : reported input params\n");
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT :
        memcpy(&resp->params, &pStream->out.params, sizeof(resp->params));
        DPRINTF("CMD_GET_PARAMS : reported output params\n");
        break;
    default :
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("CMD_GET_PARAMS : invalid queue type 0x%x", req->queue_type);
        break;
    }

    DPRINTF(
            "Get %d Params:\n"
            "format       = %s\n"
            "frame_width  = %d\n"
            "frame_height = %d\n"
            "min_buffers  = %d\n"
            "max_buffers  = %d\n"
            "cropX        = %d\n"
            "cropY        = %d\n"
            "cropW        = %d\n"
            "cropH        = %d\n"
            "frame_rate   = %d\n"
            "num_planes   = %d\n", 
            req->queue_type, 
            virtio_video_format_name(resp->params.format), 
            resp->params.frame_width, resp->params.frame_height, 
            resp->params.min_buffers, resp->params.max_buffers, 
            resp->params.crop.left  , resp->params.crop.top, 
            resp->params.crop.width , resp->params.crop.height, 
            resp->params.frame_rate , resp->params.num_planes
            );
    
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
    mfxStatus sts = MFX_ERR_NONE;
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
        DPRINTF("Set input params for stream %d \n", pStream->id);
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
        DPRINTF("Set output params for stream %d \n", pStream->id);
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
                "num_planes   = %d       <-         num_planes    = %d\n", 
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
                pPara->num_planes,   req->params.num_planes
                );

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


        DPRINTF(
                "After setting:\n"
                "format       = %s\n"
                "frame_width  = %d\n"
                "frame_height = %d\n"
                "min_buffers  = %d\n"
                "max_buffers  = %d\n"
                "cropX        = %d\n"
                "cropY        = %d\n"
                "cropW        = %d\n"
                "cropH        = %d\n"
                "frame_rate   = %d\n"
                "num_planes   = %d\n", 
                virtio_video_format_name(pPara->format), 
                pPara->frame_width, pPara->frame_height, 
                pPara->min_buffers, pPara->max_buffers, 
                pPara->crop.left  , pPara->crop.top, 
                pPara->crop.width , pPara->crop.height, 
                pPara->frame_rate , pPara->num_planes
                );

        if (pStream->in.params.format != 0 && pStream->out.params.format != 0 && 
            pStream->out.params.frame_width != 0 && pStream->out.params.frame_height != 0) {
                sts = virtio_video_msdk_init_encoder_stream(pStream);
                if (sts == MFX_ERR_NONE) {
                    pStream->state = STREAM_STATE_RUNNING;
                    virtio_video_encode_start_running(pStream);
                }
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

    DPRINTF("Init handle\n");
    if (virtio_video_msdk_init_handle(v)) {
        return -1;
    }

    DPRINTF("MFXInitEx\n");
    sts = MFXInitEx(par, &mfx_session);
    if (sts != MFX_ERR_NONE) {
        error_report("MFXInitEx returns %d", sts);
        return -1;
    }

    hdl = v->opaque;
    DPRINTF("SetHandle\n");
    sts = MFXVideoCORE_SetHandle(mfx_session, MFX_HANDLE_VA_DISPLAY, hdl->va_handle);
    if (sts != MFX_ERR_NONE) {
        error_report("MFXVideoCORE_SetHandle returns %d", sts);
        MFXClose(mfx_session);
        return -1;
    }

    /* For encoder, just query output format */
    DPRINTF("Query output capabilities\n");
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

        DPRINTF("Generate output format desc\n");
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
    DPRINTF("Query Input capabilities\n");

    for (unsigned int i = 0; i < in_fmt_nums; i++) {
        in_fmt = g_new0(VirtIOVideoFormat, 1);
        virtio_video_init_format(in_fmt, in_format[i]);

        in_fmt_frame = g_new0(VirtIOVideoFormatFrame, 1);
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
        pStream->control.bitrate = 0;
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

    qemu_mutex_lock(&pStream->mutex);
    qemu_mutex_lock(&pStream->mutex_enc);
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
        qemu_mutex_unlock(&pStream->mutex_enc);
        qemu_mutex_unlock(&pStream->mutex);
        return sizeof(virtio_video_cmd_hdr);
        break;
    default :
        break;
    }

    pCmd->elem = pElem;
    pCmd->cmd_type = VIRTIO_VIDEO_CMD_STREAM_DESTROY;
    pStream->state = STREAM_STATE_TERMINATE;
    QLIST_REMOVE(pStream, next);
    qemu_event_set(&pSession->input_notifier);
    qemu_event_set(&pSession->output_notifier);

    qemu_mutex_unlock(&pStream->mutex_enc);
    qemu_mutex_unlock(&pStream->mutex);
    return 0;
}

int virtio_video_msdk_init_encoder_stream(VirtIOVideoStream *pStream)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    mfxStatus sts = MFX_ERR_NONE;
    MsdkSession *pSession = NULL;
    mfxVideoParam enc_param = {0}; // , vpp_param = {0};
    mfxFrameAllocRequest enc_req; // , vpp_req[2];// , preenc_req[2];
    VirtIOVideoFormat *pFmt = NULL;
    VirtIOVideo *pV = NULL;
    // virtio_video_params *pInputPara = NULL, *pOutputPara = NULL;
    virtio_video_params *pOutputPara = NULL;
    bool bOutFmtSupported = false;
    if (pStream == NULL) {
        return -1;
    }

    pSession = pStream->opaque;
    pV = pStream->parent;
    // pInputPara = &pStream->in.params;
    pOutputPara = &pStream->out.params;
    /*
     * 1. Init encoder, query encode surface, prepare buffers
     * 2. Init VPP, query vpp surface, prepare buffers
     * 3. Init preenc, query preenc surface, prepare buffers
     */
    
    // Check if need vpp and preenc
    // pStream->bVpp = true;
    // pStream->bPreenc = true;
    // QLIST_FOREACH(pFmt, &pV->format_list[VIRTIO_VIDEO_QUEUE_INPUT], next) {
    //     if (pFmt->desc.format == pInputPara->format) {
    //         pStream->bVpp = false;
    //         pStream->bPreenc = false;
    //         break;
    //     }
    // }

    // pStream->bVpp = false;
    // pStream->bPreenc = false;
    

    QLIST_FOREACH(pFmt, &pV->format_list[VIRTIO_VIDEO_QUEUE_OUTPUT], next) {
        DPRINTF("pFmt->desc.format = %d, pOutputPara->format = %d\n", pFmt->desc.format, pOutputPara->format);
        if (pFmt->desc.format == pOutputPara->format) {
            bOutFmtSupported = true;
            break;
        }
    }
    if (!bOutFmtSupported) { /* The encode format is not support yet */
        error_report("The encode format %d is not support yet!!!\n", pOutputPara->format);
        return MFX_ERR_UNSUPPORTED;
    }

    if (virtio_video_msdk_init_enc_param(&enc_param, pStream) < 0) {
         return MFX_ERR_UNSUPPORTED;
    }

    switch (pOutputPara->format) {
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

    sts = MFXVideoENCODE_Init(pSession->session, &enc_param);
    if (sts != MFX_ERR_NONE) {
        error_report("stream %d MFXVideoENCODE_Init failed : %d  in encoder\n", pStream->id, 
                     sts);
        return MFX_ERR_UNSUPPORTED;
    }

    sts = MFXVideoENCODE_QueryIOSurf(pSession->session, &enc_param, &enc_req);
    if (sts != MFX_ERR_NONE) {
        error_report("stream %d MFXVideoENCODE_QueryIOSurf failed(%d)\n", pStream->id, sts);
        return MFX_ERR_UNSUPPORTED;
    }

/*
    if (pStream->bVpp) {
        if (virtio_video_msdk_init_vpp_param(&enc_param, &vpp_param, pStream) < 0) {
            MFXVideoENCODE_Close(pSession->session);
            return MFX_ERR_UNSUPPORTED;
        }

        sts = MFXVideoVPP_QueryIOSurf(pSession->session, &vpp_param, vpp_req);
        if (sts != MFX_ERR_NONE && sts != MFX_WRN_PARTIAL_ACCELERATION) {
            error_report("stream %d MFXVideoVPP_QuerySurf failed(%d)\n", pStream->id, sts);
            MFXVideoENCODE_Close(pSession->session);
            return MFX_ERR_UNSUPPORTED;
        }

        sts = MFXVideoVPP_Init(pSession->session, &vpp_param);
        if (sts != MFX_ERR_NONE && sts != MFX_WRN_PARTIAL_ACCELERATION) {
            error_report("stream %d MFXVideoVPP_Init failed(%d)\n", pStream->id, sts);
            MFXVideoENCODE_Close(pSession->session);
            return MFX_ERR_UNSUPPORTED;
        }

        pSession->surface_num = vpp_req[1].NumFrameSuggested;
        pSession->vpp_surface_num = vpp_req[0].NumFrameSuggested;

        virtio_video_msdk_init_surface_pool(pSession, &vpp_req[0], &vpp_param.vpp.In, true);
    }
    */

    pSession->surface_num += enc_req.NumFrameSuggested;
    virtio_video_msdk_init_surface_pool(pSession, &enc_req, &enc_param.mfx.FrameInfo, false);


    pStream->in.params.min_buffers = enc_req.NumFrameMin;
    pStream->in.params.max_buffers = enc_req.NumFrameSuggested;
    pStream->out.params.min_buffers = enc_req.NumFrameMin;
    pStream->out.params.max_buffers = enc_req.NumFrameSuggested;
    virtio_video_msdk_stream_reset_param(pStream, &enc_param, true);
    virtio_video_msdk_stream_reset_param(pStream, &enc_param, false);

    return MFX_ERR_NONE;
}

int virtio_video_msdk_init_enc_param(mfxVideoParam *pPara, VirtIOVideoStream *pStream)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    virtio_video_params *pOutPara = NULL;
    VirtIOVideoEncodeParamPreset vvepp = {0};
    mfxExtCodingOption2 *pOpt2 = NULL;
    // VirtIOVideoEncodeParamPreset ParamPreset = {0};
    if (pPara == NULL || pStream == NULL)
        return -2;

    pOutPara = &(pStream->out.params);
    if (pOutPara->frame_rate == 0) pOutPara->frame_rate = 30;

    // H.264 encode param init
    // TODO : Added support for other formats.
    memset(pPara, 0, sizeof(mfxVideoParam));

    pPara->mfx.CodecId                 = virtio_video_format_to_msdk(pOutPara->format);
    virtio_video_msdk_get_preset_param_enc(&vvepp, pPara->mfx.CodecId, pOutPara->frame_rate, pOutPara->frame_width, pOutPara->frame_height);
    pPara->mfx.TargetUsage             = vvepp.epp.TargetUsage;
    pPara->mfx.RateControlMethod       = vvepp.epp.RateControlMethod;
    pPara->mfx.GopRefDist              = vvepp.epp.GopRefDist;
    pPara->mfx.GopPicSize              = vvepp.dpp.GopPicSize;
    pPara->mfx.NumRefFrame             = 0;
    pPara->mfx.IdrInterval             = 0;

    pPara->mfx.CodecProfile            = 0;
    pPara->mfx.CodecLevel              = 0;
    pPara->mfx.MaxKbps                 = vvepp.dpp.MaxKbps;
    pPara->mfx.InitialDelayInKB        = 0;
    pPara->mfx.GopOptFlag              = 0;
    pPara->mfx.BufferSizeInKB          = vvepp.dpp.BufferSizeInKB;

    if (pPara->mfx.RateControlMethod == MFX_RATECONTROL_CQP) {
        pPara->mfx.QPI                 = 0;
        pPara->mfx.QPB                 = 0;
        pPara->mfx.QPP                 = 0;
    } else if (pPara->mfx.RateControlMethod == MFX_RATECONTROL_ICQ || 
        pPara->mfx.RateControlMethod == MFX_RATECONTROL_LA_ICQ) {
        pPara->mfx.ICQQuality          = 0;
    } else if (pPara->mfx.RateControlMethod == MFX_RATECONTROL_AVBR) {

    } else {
        pPara->mfx.TargetKbps = vvepp.dpp.TargetKbps;
    }

    pPara->mfx.NumSlice                = 0;

    virtio_video_msdk_convert_frame_rate(pOutPara->frame_rate, &(pPara->mfx.FrameInfo.FrameRateExtN), 
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

    if (pPara->mfx.CodecId == MFX_CODEC_AVC) {
        pPara->ExtParam = g_malloc0(sizeof(mfxExtBuffer *));
        pOpt2 = g_new0(mfxExtCodingOption2, 1);

        pOpt2->LookAheadDepth          = 0;
        pOpt2->MaxSliceSize            = 0;
        pOpt2->MaxFrameSize            = 0;
        pOpt2->BRefType                = vvepp.epp.BRefType;
        pOpt2->BitrateLimit            = MFX_CODINGOPTION_OFF;

        pOpt2->ExtBRC                  = 0;

        pOpt2->IntRefType              = vvepp.epp.IntRefType;
        pOpt2->IntRefCycleSize         = vvepp.epp.IntRefCycleSize;
        pOpt2->IntRefQPDelta           = vvepp.epp.IntRefQPDelta;
        pOpt2->AdaptiveI               = 0;
        pOpt2->AdaptiveB               = 0;
        pOpt2->Header.BufferId         = MFX_EXTBUFF_CODING_OPTION2;
        pOpt2->Header.BufferSz         = sizeof(*pOpt2);
        pPara->ExtParam[0]             = (mfxExtBuffer *)pOpt2;
        pPara->NumExtParam             += 1;
    }


    pPara->AsyncDepth                  = vvepp.epp.AsyncDepth;
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

mfxStatus virtio_video_encode_submit_one_frame(VirtIOVideoWork *pWork)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = pWork->parent;
    MsdkSession *pSession = pStream->opaque;
    MsdkFrame *pFrame = pWork->opaque;
    MsdkSurface *pWorkSurf = NULL;
    mfxBitstream *pOutBs = NULL;
    mfxStatus sts = MFX_ERR_NONE;

    // if (pStream->bVpp) {
    //     QLIST_FOREACH(pWorkSurf, &pSession->vpp_surface_pool, next) {
    //         if (!pWorkSurf->used && !pWorkSurf->surface.Data.Locked) {
    //             pWorkSurf->used = true;
    //             pFrame->vpp_surface = pWorkSurf;
    //             break;
    //         }
    //     }

    //     if (pWorkSurf == NULL) {
    //         error_report("%s %s : virtio-video: stream %d no available surface "
    //                   "in vpp surface pool", __FILE__, __FUNCTION__, pStream->id);
    //          return MFX_ERR_NOT_ENOUGH_BUFFER;
    //     }

    //     do {
    //         sts = MFXVideoVPP_RunFrameVPPAsync(pSession->session, &pFrame->surface->surface, 
    //                 &pFrame->vpp_surface->surface, NULL, &pFrame->vpp_sync);
    //                 // &pFrame->vpp_surface->surface, NULL, &pFrame->vpp_sync);
    //         switch (sts) {
    //         case MFX_WRN_DEVICE_BUSY :
    //             usleep(1000);
    //             break;
    //         case MFX_ERR_NONE:
    //             break;
    //         default:
    //             error_report("%s %s : virtio-video: stream %d input resource %d "
    //                      "MFXVideoVPP_RunFrameVPPAsync failed: %d", __FILE__, __FUNCTION__, 
    //                      pStream->id, pWork->resource->id, sts);
    //             return sts;
    //         } 
    //     } while (sts == MFX_WRN_DEVICE_BUSY);
    // }
    // // TODO: Add Preenc Process
    if (pWorkSurf == NULL) {
        pWorkSurf = pFrame->surface;
    }

    if (pFrame->pBitStream == NULL) {
        pFrame->pBitStream = g_new0(mfxBitstream, 1);
    }

    pOutBs = pFrame->pBitStream;
    do {
        sts = MFXVideoENCODE_EncodeFrameAsync(pSession->session, NULL, &pWorkSurf->surface, 
                                          pOutBs, &pFrame->sync);
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

    return MFX_ERR_NONE;
}

int virtio_video_encode_fill_input_data(MsdkFrame *pFrame, VirtIOVideoResource *pRes, uint32_t format)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    mfxFrameSurface1 *pSurf = NULL;
    mfxFrameData *pData = NULL;
    bool bSuccess = true;
    if (pFrame == NULL || pRes == NULL) return -1;
    pSurf = &(pFrame->surface->surface);
    pData = &(pSurf->Data);
    switch (pSurf->Info.FourCC) {
    case MFX_FOURCC_RGB4 :
        if (pRes->num_planes != 1) {
            bSuccess = false;
        }
        pData->R = pRes->slices[0]->page.base;
        pData->G = pData->R + 1;
        pData->B = pData->G + 1;
        break;
    case MFX_FOURCC_NV12 :
        if (pRes->num_planes != 2) {
            bSuccess = false;
        }
        pData->Y = pRes->slices[0]->page.base;
        pData->U = pRes->slices[1]->page.base;
        break;
    case MFX_FOURCC_IYUV :
    if (pRes->num_planes != 3) {
            bSuccess = false;
        }
        pData->Y = pRes->slices[0]->page.base;
        pData->U = pRes->slices[1]->page.base;
        pData->V = pRes->slices[2]->page.base;
        break;
    case MFX_FOURCC_YV12 :
    if (pRes->num_planes != 3) {
            bSuccess = false;
        }
        pData->Y = pRes->slices[0]->page.base;
        pData->V = pRes->slices[1]->page.base;
        pData->U = pRes->slices[2]->page.base;
        break;
    default:
        bSuccess = false;
        break;
    }

    pFrame->surface->used = bSuccess ? true : false;
    return bSuccess ? 0 : -1;
}

int virtio_video_encode_retrieve_one_frame(VirtIOVideoWork *pWorkOut)
{
#ifdef CALL_NO_DEBUG
    DPRINTF("%s : CALL_No = %d\n", __FUNCTION__, CALL_No++);
#endif
    VirtIOVideoStream *pStream = pWorkOut->parent;
    MsdkSession *pSession = pStream->opaque;
    MsdkFrame *pFrameOut = pWorkOut->opaque;
    mfxStatus sts = MFX_ERR_NONE;
    // int ret = 0;

    do {
        sts = MFXVideoCORE_SyncOperation(pSession->session, pFrameOut->sync, VIRTIO_VIDEO_MSDK_TIME_TO_WAIT);
        if (sts == MFX_WRN_IN_EXECUTION && 
                (pStream->state == STREAM_STATE_TERMINATE)) {
                     virtio_video_encode_clear_work(pWorkOut);
                     return -1;
                 }
    } while (sts == MFX_WRN_IN_EXECUTION);

    if (sts != MFX_ERR_NONE) {
        error_report("virtio-video-encode/%d MFXVideoCORE_SyncOperation "
                      "failed: %d", pStream->id, sts); 
        virtio_video_encode_clear_work(pWorkOut);
        return -1;
    } else {

    }

    g_free(pWorkOut->opaque);
    virtio_video_work_done(pWorkOut);
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
    qemu_mutex_lock(&pStream->mutex);
    switch (queue_type) {
    case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
        switch (pStream->state) {
        case STREAM_STATE_INIT:
            /*
             * CMD_STREAM_DRAIN can be pending, waiting for the completion of
             * initialization phase. Then, it can do decoding of all pending
             * works in @input_work. We simply drop those works for
             * CMD_QUEUE_CLEAR and CMD_RESOURCE_DESTROY_ALL.
             */
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
        break;
    case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
        if (pStream->state == STREAM_STATE_TERMINATE) {
            assert(pCmd->cmd_type == VIRTIO_VIDEO_CMD_STREAM_DESTROY);
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
            DPRINTF("%s: stream %d currently unable to serve the request",
                    destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                    pStream->id);
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
        break;
    default:
        resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
        error_report("%s: invalid queue type 0x%x",
                     destroy ? "CMD_RESOURCE_DESTROY_ALL" : "CMD_QUEUE_CLEAR",
                     queue_type);
        break;
    }

    qemu_mutex_unlock(&pStream->mutex);
    return len;
}
>>>>>>> 3d36f6e9dd (Merge encode part from WanHao and Shenlin)
