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
#include "virtio-video-msdk.h"
#include "virtio-video-msdk-dec.h"
#include "virtio-video-msdk-vaapi.h"
#include "virtio-video-msdk-util.h"
#include "mfx/mfxvideo.h"

#define THREAD_NAME_LEN 24

static void *virtio_video_decode_thread(void *arg)
{
    VirtIOVideoStream *stream = arg;
    VirtIOVideoStreamMediaSDK *msdk = stream->opaque;
    int i;
    bool running = true, decoding = true;
    mfxStatus sts = MFX_ERR_NONE;
    mfxFrameAllocRequest allocRequest, vppRequest[2];
    mfxU16 numSurfaces;
    mfxU8* surfaceBuffers;
    mfxFrameSurface1 *surface_work;
    mfxFrameSurface1 *surface_nv12;
    mfxSyncPoint syncp, syncVpp;
    mfxVideoParam VPPParams = {0};

    /* Prepare an initial mfxVideoParam for decode */
    virtio_video_msdk_load_plugin(msdk->session, stream->in.params.format, false);
    virtio_video_msdk_init_param_dec(&msdk->param, stream);

    sts = MFXVideoDECODE_Init(msdk->session, &msdk->param);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_Init failed with err %d", stream->id, sts);
    }

    /* Retrieve current working mfxVideoParam */
    sts = MFXVideoDECODE_GetVideoParam(msdk->session, &msdk->param);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_GetVideoParam failed with err %d", stream->id, sts);
    }

    /* Query and allocate working surface */
    memset(&allocRequest, 0, sizeof(allocRequest));
    sts = MFXVideoDECODE_QueryIOSurf(msdk->session, &msdk->param, &allocRequest);
    if (sts != MFX_ERR_NONE && sts != MFX_WRN_PARTIAL_ACCELERATION) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_QueryIOSurf failed with err %d", stream->id, sts);
        running = false;
    } else {
        mfxU16 width = (mfxU16) MSDK_ALIGN32(allocRequest.Info.Width);
        mfxU16 height = (mfxU16) MSDK_ALIGN32(allocRequest.Info.Height);
        mfxU8 bitsPerPixel = 12; /* NV12 format is a 12 bits per pixel format */
        mfxU32 surfaceSize = width * height * bitsPerPixel / 8;

        numSurfaces = allocRequest.NumFrameSuggested;
        surfaceBuffers = g_malloc0(surfaceSize * numSurfaces);
        surface_work = g_malloc0(numSurfaces * sizeof(mfxFrameSurface1));
        if (surfaceBuffers && surface_work) {
            for (i = 0; i < numSurfaces; i++) {
                surface_work[i].Info = msdk->param.mfx.FrameInfo;
                surface_work[i].Data.Y = &surfaceBuffers[surfaceSize * i];
                surface_work[i].Data.U = surface_work[i].Data.Y + width * height;
                surface_work[i].Data.V = surface_work[i].Data.U + 1;
                surface_work[i].Data.Pitch = width;
            }
        } else {
            VIRTVID_ERROR("stream 0x%x allocate working surface failed", stream->id);
            running = false;
        }
    }

    /* Prepare VPP Params for color space conversion */
    memset(&VPPParams, 0, sizeof(VPPParams));
    VPPParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    /* Input */
    VPPParams.vpp.In.FourCC = MFX_FOURCC_NV12;
    VPPParams.vpp.In.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    VPPParams.vpp.In.CropX = 0;
    VPPParams.vpp.In.CropY = 0;
    VPPParams.vpp.In.CropW = msdk->param.mfx.FrameInfo.CropW;
    VPPParams.vpp.In.CropH = msdk->param.mfx.FrameInfo.CropH;
    VPPParams.vpp.In.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    VPPParams.vpp.In.FrameRateExtN = msdk->param.mfx.FrameInfo.FrameRateExtN;
    VPPParams.vpp.In.FrameRateExtD = msdk->param.mfx.FrameInfo.FrameRateExtD;
    VPPParams.vpp.In.Height = (MFX_PICSTRUCT_PROGRESSIVE == VPPParams.vpp.In.PicStruct) ?
        MSDK_ALIGN16(msdk->param.mfx.FrameInfo.Height) : MSDK_ALIGN32(msdk->param.mfx.FrameInfo.Height);
    VPPParams.vpp.In.Width = (MFX_PICSTRUCT_PROGRESSIVE == VPPParams.vpp.In.PicStruct) ?
        MSDK_ALIGN16(msdk->param.mfx.FrameInfo.Width) :  MSDK_ALIGN32(msdk->param.mfx.FrameInfo.Width);
    /* Output */
    memcpy(&VPPParams.vpp.Out, &VPPParams.vpp.In, sizeof(VPPParams.vpp.Out));
    VPPParams.vpp.Out.FourCC = virtio_video_format_to_msdk(stream->out.params.format);
    VPPParams.vpp.Out.ChromaFormat = 0;

    /* Query and allocate VPP surface */
    memset(&vppRequest, 0, sizeof(vppRequest));
    sts = MFXVideoVPP_QueryIOSurf(msdk->session, &VPPParams, vppRequest);
    if (sts != MFX_ERR_NONE && sts != MFX_WRN_PARTIAL_ACCELERATION) {
        VIRTVID_ERROR("stream 0x%x MFXVideoVPP_QueryIOSurf failed with err %d", stream->id, sts);
        running = false;
    } else {
        msdk->surface.Info = VPPParams.vpp.Out;
        msdk->surface.Data.Pitch = ((mfxU16)MSDK_ALIGN32(vppRequest[1].Info.Width)) * 32 / 8;
    }

    sts = MFXVideoVPP_Init(msdk->session, &VPPParams);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoVPP_Init failed with err %d", stream->id, sts);
    }

    while (running) {
        mfxFrameSurface1 *surf = NULL;

        decoding = false;

        qemu_event_wait(&msdk->signal_in);
        qemu_event_reset(&msdk->signal_in);

        qemu_mutex_lock(&stream->mutex);
        if (!QLIST_EMPTY(&stream->ev_list)) {
            VirtIOVideoStreamEventEntry *entry = QLIST_FIRST(&stream->ev_list);

            QLIST_SAFE_REMOVE(entry, next);

            switch (entry->ev) {
            case VirtIOVideoStreamEventStreamDrain:
                /* set bs to NULL to signal end of stream to drain the decoding */
                do {
                    sts = MFXVideoDECODE_DecodeFrameAsync(msdk->session, NULL, surface_work, &surface_nv12, &syncp);
                    MFXVideoCORE_SyncOperation(msdk->session, syncp, stream->mfxWaitMs);
                } while (sts != MFX_ERR_MORE_DATA && (--stream->retry) > 0);
                MFXVideoVPP_Reset(msdk->session, &VPPParams);
                MFXVideoDECODE_Reset(msdk->session, &msdk->param);
                break;
            case VirtIOVideoStreamEventResourceQueue:
                decoding = true;
                break;
            case VirtIOVideoStreamEventQueueClear:
                decoding = false;
                running = false;
                /* TODO: How to clear queue? */
                if (*(uint32_t*)(entry->data) == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {

                } else { /* VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT */

                }
                break;
            case VirtIOVideoStreamEventTerminate:
                running = false;
                break;
            default:
                break;
            }

            g_free(entry);
        }
        qemu_mutex_unlock(&stream->mutex);

        if (!running || !decoding) {
            continue;
        }

        for (i = 0; i < numSurfaces; i++) {
            if (!surface_work[i].Data.Locked) {
                surf = &surface_work[i];
                break;
            }
        }

        if (!surf) {
            VIRTVID_ERROR("No free surface_work");
            continue;
        }

        sts = MFXVideoDECODE_DecodeFrameAsync(msdk->session, &msdk->bitstream, surf, &surface_nv12, &syncp);
        if (sts == MFX_ERR_NONE) {
            sts = MFXVideoCORE_SyncOperation(msdk->session, syncp, stream->mfxWaitMs);
            if (sts == MFX_ERR_NONE) {
                stream->stat = VirtIOVideoStreamStatNone;
            } else {
                running = false;
                stream->stat = VirtIOVideoStreamStatError;
                VIRTVID_ERROR("stream 0x%x MFXVideoCORE_SyncOperation failed with err %d", stream->id, sts);
            }
        } else {
            running = false;
            stream->stat = VirtIOVideoStreamStatError;
            VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_DecodeFrameAsync failed with err %d", stream->id, sts);
        }

        for (;;) {
            sts = MFXVideoVPP_RunFrameVPPAsync(msdk->session, surface_nv12, &msdk->surface, NULL, &syncVpp);
            if (sts > MFX_ERR_NONE&& !syncVpp) {
                if (sts == MFX_WRN_DEVICE_BUSY) {
                    g_usleep(1000);
                }
            } else if (sts > MFX_ERR_NONE && syncVpp) {
                sts = MFX_ERR_NONE;
                break;
            } else
            break;
        }

        /* Notify CMD_RESOURCE_QUEUE, it's waiting for virtio_video_resource_queue_resp */
        qemu_event_set(&msdk->signal_out);
    }

    sts = MFXVideoVPP_Reset(msdk->session, &VPPParams);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoVPP_Reset failed with err %d", stream->id, sts);
    }

    sts = MFXVideoVPP_Close(msdk->session);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoVPP_Close failed with err %d", stream->id, sts);
    }

    sts = MFXVideoDECODE_Reset(msdk->session, &msdk->param);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_Reset failed with err %d", stream->id, sts);
    }

    sts = MFXVideoDECODE_Close(msdk->session);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_Close failed with err %d", stream->id, sts);
    }


    g_free(surfaceBuffers);
    g_free(surface_work);

    return NULL;
}

size_t virtio_video_msdk_dec_stream_create(VirtIOVideo *v,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideoFormat *fmt;
    VirtIOVideoStream *stream;
    VirtIOVideoStreamMediaSDK *msdk;
    mfxStatus status;
    char thread_name[THREAD_NAME_LEN];
    size_t len;

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

    msdk = g_new(VirtIOVideoStreamMediaSDK, 1);
    if (msdk == NULL)
        return len;

    status = MFXInitEx(param, &msdk->session);
    if (status != MFX_ERR_NONE) {
        VIRTVID_ERROR("    %s: MFXInitEx returns %d for stream 0x%x", __func__,
                status, resp->stream_id);
        g_free(msdk);
        return len;
    }

    status = MFXVideoCORE_SetHandle(msdk->session, MFX_HANDLE_VA_DISPLAY,
                                    ((VirtIOVideoMediaSDK *)v->opaque)->va_handle);
    if (status != MFX_ERR_NONE) {
        VIRTVID_ERROR("    %s: MFXVideoCORE_SetHandle returns %d for stream 0x%x",
                __func__, status, resp->stream_id);
        MFXClose(msdk->session);
        g_free(msdk);
        return len;
    }

    stream = g_new0(VirtIOVideoStream, 1);
    if (stream == NULL) {
        g_free(msdk);
        return len;
    }
    stream->opaque = msdk;

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

    /*
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

    QLIST_INIT(&stream->resource_list[VIRTIO_VIDEO_RESOURCE_LIST_INPUT]);
    QLIST_INIT(&stream->resource_list[VIRTIO_VIDEO_RESOURCE_LIST_OUTPUT]);

    QLIST_INIT(&stream->ev_list);
    qemu_event_init(&msdk->signal_in, false);
    qemu_event_init(&msdk->signal_out, false);
    stream->mfxWaitMs = 60000;
    stream->state = STREAM_STATE_INIT;
    stream->stat = VirtIOVideoStreamStatNone;

    qemu_mutex_init(&stream->mutex);

    snprintf(thread_name, sizeof(thread_name), "virtio-video-decode/%d",
             stream->id);
    qemu_thread_create(&msdk->thread, thread_name, virtio_video_decode_thread,
                       stream, QEMU_THREAD_JOINABLE);

    QLIST_INSERT_HEAD(&v->stream_list, stream, next);
    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;

    VIRTVID_DEBUG("    %s: stream 0x%x created", __func__, stream->id);
    return len;
}

size_t virtio_video_msdk_dec_stream_destroy(VirtIOVideo *v,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideoStream *stream, *next = NULL;
    VirtIOVideoStreamMediaSDK *msdk;
    VirtIOVideoStreamEventEntry *entry, *next_entry = NULL;
    VirtIOVideoResource *res, *next_res = NULL;
    size_t len = 0;
    int i;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, next) {
        if (stream->id == req->hdr.stream_id) {
            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            msdk = stream->opaque;

            entry = g_new0(VirtIOVideoStreamEventEntry, 1);
            entry->ev = VirtIOVideoStreamEventTerminate;
            qemu_mutex_lock(&stream->mutex);
            QLIST_INSERT_HEAD(&stream->ev_list, entry, next);
            qemu_mutex_unlock(&stream->mutex);
            qemu_event_set(&msdk->signal_in);

            qemu_thread_join(&msdk->thread);
            msdk->thread.thread = 0;

            QLIST_FOREACH_SAFE(entry, &stream->ev_list, next, next_entry) {
                QLIST_SAFE_REMOVE(entry, next);
                g_free(entry);
            }

            for (i = 0; i < VIRTIO_VIDEO_RESOURCE_LIST_NUM; i++) {
                QLIST_FOREACH_SAFE(res, &stream->resource_list[i], next, next_res) {
                    QLIST_SAFE_REMOVE(res, next);
                    g_free(res);
                }
            }

            qemu_event_destroy(&msdk->signal_in);
            qemu_event_destroy(&msdk->signal_out);

            qemu_mutex_destroy(&stream->mutex);

            virtio_video_msdk_unload_plugin(msdk->session, stream->in.params.format, false);
            MFXClose(msdk->session);
            g_free(msdk);

            QLIST_SAFE_REMOVE(stream, next);
            g_free(stream);
            VIRTVID_DEBUG("    %s: stream 0x%x destroyed", __func__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_msdk_dec_stream_drain(VirtIOVideo *v,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideoStream *stream, *next = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, next) {
        if (stream->id == req->hdr.stream_id) {
            VirtIOVideoStreamMediaSDK *msdk = stream->opaque;
            VirtIOVideoStreamEventEntry *entry = g_new0(VirtIOVideoStreamEventEntry, 1);

            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            entry->ev = VirtIOVideoStreamEventStreamDrain;
            qemu_mutex_lock(&stream->mutex);
            /* Set retry count for drain */
            stream->retry = 10;
            QLIST_INSERT_HEAD(&stream->ev_list, entry, next);
            qemu_mutex_unlock(&stream->mutex);
            qemu_event_set(&msdk->signal_in);
            VIRTVID_DEBUG("    %s: stream 0x%x drained", __func__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_msdk_dec_resource_queue(VirtIOVideo *v,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp)
{
    VirtIOVideoStream *stream;
    VirtIOVideoStreamMediaSDK *msdk;
    size_t len = 0;

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

    VirtIOVideoResource *res, *next_res = NULL;
    VirtIOVideoStreamEventEntry *entry = g_new0(VirtIOVideoStreamEventEntry, 1);

    msdk = stream->opaque;
    if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
        QLIST_FOREACH_SAFE(res,
                &stream->resource_list[VIRTIO_VIDEO_RESOURCE_LIST_OUTPUT], next,
                next_res) {
            if (req->resource_id == res->id) {
                /* Set mfxSurfOut buffer to the request hva, decode thread will fill other parameters */
                msdk->surface.Data.Y = res->slices[0][0].page.hva;
                resp->hdr.type = VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE;
            }
        }
    } else if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
        QLIST_FOREACH_SAFE(res,
                &stream->resource_list[VIRTIO_VIDEO_RESOURCE_LIST_INPUT], next,
                next_res) {
            if (req->resource_id == res->id) {
                /* bitstream shouldn't have plane concept */
                msdk->bitstream.MaxLength = req->data_sizes[0];
                msdk->bitstream.Data = res->slices[0][0].page.hva;
                resp->hdr.type = VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE;

                /* Notify decode thread to start on new input resource queued */
                entry->ev = VirtIOVideoStreamEventResourceQueue;
                qemu_mutex_lock(&stream->mutex);
                QLIST_INSERT_HEAD(&stream->ev_list, entry, next);
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_set(&msdk->signal_in);

                /* Wait for decode thread work done */
                qemu_event_wait(&msdk->signal_out);
                qemu_event_reset(&msdk->signal_out);
            }
        }
    } else {
        resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
        VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x",
                __func__, req->hdr.stream_id, req->queue_type);
    }

    resp->timestamp = req->timestamp;
    resp->size = 0; /* Only for encode */
    if (stream->stat == VirtIOVideoStreamStatError) {
        resp->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
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

    /* TODO: Drain codec */
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
    VirtIOVideoStream *stream, *next = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, next) {
        if (stream->id == req->hdr.stream_id) {
            VirtIOVideoStreamMediaSDK *msdk = stream->opaque;
            VirtIOVideoStreamEventEntry *entry = g_new0(VirtIOVideoStreamEventEntry, 1);

            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT || req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                entry->ev = VirtIOVideoStreamEventQueueClear;
                entry->data = g_malloc(sizeof(uint32_t));
                *(uint32_t*)(entry->data) = req->queue_type;

                qemu_mutex_lock(&stream->mutex);
                QLIST_INSERT_HEAD(&stream->ev_list, entry, next);
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_set(&msdk->signal_in);
            } else {
                resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                g_free(entry);
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x",
                        __func__, req->hdr.stream_id, req->queue_type);
            }

            VIRTVID_DEBUG("    %s: stream 0x%x queue_type 0x%x cleared",
                    __func__, req->hdr.stream_id, req->queue_type);
            break;
        }
    }

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

    if (virtio_video_init_msdk_handle(v)) {
        VIRTVID_ERROR("Fail to create VA environment on DRM");
        return -1;
    }

    status = MFXInitEx(init_param, &session);
    if (status != MFX_ERR_NONE) {
        VIRTVID_ERROR("MFXInitEx returns %d", status);
        return -1;
    }

    status = MFXVideoCORE_SetHandle(session, MFX_HANDLE_VA_DISPLAY,
                                    ((VirtIOVideoMediaSDK *)v->opaque)->va_handle);
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
        virtio_video_msdk_init_format(in_fmt, in_format);

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
        virtio_video_msdk_init_format(out_fmt, out_format[i]);

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

    virtio_video_uninit_msdk_handle(v);
}
