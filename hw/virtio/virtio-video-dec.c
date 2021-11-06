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
#include "exec/address-spaces.h"
#include "virtio-video-dec.h"
#include "virtio-video-msdk.h"
#include "virtio-video-vaapi.h"

#define VIRTIO_VIDEO_DECODE_THREAD "Virtio-Video-Decode"

static void *virtio_video_decode_thread(void *arg)
{
    VirtIOVideoStream *stream = arg;
    VirtIOVideoStreamMediaSDK *msdk = stream->opaque;
    sigset_t sigmask, old;
    int err, i;
    bool running = true, decoding = true;
    mfxStatus sts = MFX_ERR_NONE;
    mfxFrameAllocRequest allocRequest, vppRequest[2];
    mfxU16 numSurfaces;
    mfxU8* surfaceBuffers;
    mfxFrameSurface1 *surface_work;
    mfxFrameSurface1 *surface_nv12;
    mfxSyncPoint syncp, syncVpp;
    mfxVideoParam VPPParams = {0};

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGINT);
    err = pthread_sigmask(SIG_BLOCK, &sigmask, &old);
    if (err) {
        VIRTVID_ERROR("%s thread 0x%0x change SIG_BLOCK failed err %d", VIRTIO_VIDEO_DECODE_THREAD, stream->id, err);
    }

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
    VPPParams.vpp.Out.FourCC = virtio_video_format_to_msdk(stream->out_params.format);
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

    VIRTVID_DEBUG("%s thread 0x%0x running", VIRTIO_VIDEO_DECODE_THREAD, stream->id);
    while (running) {
        mfxFrameSurface1 *surf = NULL;

        decoding = false;

        qemu_event_wait(&stream->signal_in);
        qemu_event_reset(&stream->signal_in);

        qemu_mutex_lock(&stream->mutex);
        if (!QLIST_EMPTY(&stream->ev_list)) {
            VirtIOVideoStreamEventEntry *entry = QLIST_FIRST(&stream->ev_list);

            QLIST_SAFE_REMOVE(entry, next);

            switch (entry->ev) {
            case VirtIOVideoStreamEventParamChange:
                sts = MFXVideoDECODE_Reset(msdk->session, &msdk->param);
                if (sts != MFX_ERR_NONE) {
                    running = false;
                }
                break;
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
        qemu_event_set(&stream->signal_out);
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

    err = pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (err) {
        VIRTVID_ERROR("%s thread 0x%0x restore old sigmask failed err %d", VIRTIO_VIDEO_DECODE_THREAD, stream->id, err);
    }

    g_free(surfaceBuffers);
    g_free(surface_work);

    VIRTVID_DEBUG("%s thread 0x%0x exits", VIRTIO_VIDEO_DECODE_THREAD, stream->id);

    return NULL;
}

size_t virtio_video_dec_cmd_stream_create(VirtIODevice *vdev,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoFormat *fmt;
    VirtIOVideoStream *stream;
    VirtIOVideoStreamMediaSDK *msdk;
    mfxStatus status;
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

    msdk = g_malloc(sizeof(VirtIOVideoStreamMediaSDK));
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
                                    ((VirtIOVideoMediaSDK *)v->opaque)->va_disp_handle);
    if (status != MFX_ERR_NONE) {
        VIRTVID_ERROR("    %s: MFXVideoCORE_SetHandle returns %d for stream 0x%x",
                __func__, status, resp->stream_id);
        MFXClose(msdk->session);
        g_free(msdk);
        return len;
    }

    stream = g_malloc0(sizeof(VirtIOVideoStream));
    if (stream == NULL) {
        g_free(msdk);
        return len;
    }
    stream->opaque = msdk;

    virtio_video_msdk_load_plugin(msdk->session, req->coded_format, false);

    stream->parent = v;
    stream->id = req->hdr.stream_id;
    stream->in_mem_type = req->in_mem_type;
    stream->out_mem_type = req->out_mem_type;
    stream->codec = req->coded_format;
    memcpy(stream->tag, req->tag, strlen((char *)req->tag));

    QLIST_INIT(&stream->ev_list);
    stream->mfxWaitMs = 60000;

    /* Prepare an initial mfxVideoParam for decode */
    virtio_video_msdk_init_video_params(&msdk->param, req->coded_format);

    /* TODO: Should we use VIDEO_MEMORY for virtio-gpu object? */
    msdk->param.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

    memset(&stream->in_params, 0, sizeof(stream->in_params));

    stream->in_params.frame_width = fmt->frames.lh_first->frame.width.max;
    stream->in_params.frame_height = fmt->frames.lh_first->frame.height.max;
    stream->in_params.min_buffers = 1;
    stream->in_params.max_buffers = 1;
    stream->in_params.crop.left = 0;
    stream->in_params.crop.top = 0;
    stream->in_params.crop.width = stream->in_params.frame_width;
    stream->in_params.crop.height = stream->in_params.frame_height;
    stream->in_params.frame_rate = fmt->frames.lh_first->frame_rates->max;

    memcpy(&stream->out_params, &stream->in_params, sizeof(stream->in_params));

    /* For VIRTIO_VIDEO_QUEUE_TYPE_INPUT */
    stream->in_params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    stream->in_params.format = stream->codec;
    /* TODO: what's the definition of plane number, size and stride for coded format? */
    stream->in_params.num_planes = 1;
    stream->in_params.plane_formats[0].plane_size = 0;
    stream->in_params.plane_formats[0].stride = 0;

    /*
     * For VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT
     * Front end doesn't support NV12 but only RGB*, while MediaSDK can only decode to NV12
     * So we let front end aware of RGB* only, use VPP to convert from NV12 to RGB*
     */
    stream->out_params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    stream->out_params.format = VIRTIO_VIDEO_FORMAT_ARGB8888;
    stream->out_params.num_planes = 1;
    stream->out_params.plane_formats[0].plane_size =
        stream->out_params.frame_width * stream->out_params.frame_height * 4;
    stream->out_params.plane_formats[0].stride = stream->out_params.frame_width * 4;

    msdk->param.mfx.FrameInfo.Width = stream->in_params.frame_width;
    msdk->param.mfx.FrameInfo.Height = stream->in_params.frame_height;
    msdk->param.mfx.FrameInfo.FrameRateExtN = stream->in_params.frame_rate;
    msdk->param.mfx.FrameInfo.FrameRateExtD = 1;

    QLIST_INIT(&stream->resource_list[VIRTIO_VIDEO_RESOURCE_LIST_INPUT]);
    QLIST_INIT(&stream->resource_list[VIRTIO_VIDEO_RESOURCE_LIST_OUTPUT]);

    qemu_event_init(&stream->signal_in, false);
    qemu_event_init(&stream->signal_out, false);
    stream->stat = VirtIOVideoStreamStatNone;

    qemu_mutex_init(&stream->mutex);

    qemu_thread_create(&stream->thread, VIRTIO_VIDEO_DECODE_THREAD,
            virtio_video_decode_thread, stream, QEMU_THREAD_JOINABLE);

    QLIST_INSERT_HEAD(&v->stream_list, stream, next);
    resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;

    VIRTVID_DEBUG("    %s: stream 0x%x created", __func__, stream->id);
    return len;
}

size_t virtio_video_dec_cmd_stream_destroy(VirtIODevice *vdev,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
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

            entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));
            entry->ev = VirtIOVideoStreamEventTerminate;
            qemu_mutex_lock(&stream->mutex);
            QLIST_INSERT_HEAD(&stream->ev_list, entry, next);
            qemu_mutex_unlock(&stream->mutex);
            qemu_event_set(&stream->signal_in);

            /* May need send SIGTERM if the thread is dead */
            //pthread_kill(stream->thread.thread, SIGTERM);
            qemu_thread_join(&stream->thread);
            stream->thread.thread = 0;

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

            qemu_event_destroy(&stream->signal_in);
            qemu_event_destroy(&stream->signal_out);

            qemu_mutex_destroy(&stream->mutex);

            msdk = stream->opaque;
            virtio_video_msdk_unload_plugin(msdk->session, stream->codec, false);
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

size_t virtio_video_dec_cmd_stream_drain(VirtIODevice *vdev,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *stream, *next = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, next) {
        if (stream->id == req->hdr.stream_id) {
            VirtIOVideoStreamEventEntry *entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));

            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            entry->ev = VirtIOVideoStreamEventStreamDrain;
            qemu_mutex_lock(&stream->mutex);
            /* Set retry count for drain */
            stream->retry = 10;
            QLIST_INSERT_HEAD(&stream->ev_list, entry, next);
            qemu_mutex_unlock(&stream->mutex);
            qemu_event_set(&stream->signal_in);
            VIRTVID_DEBUG("    %s: stream 0x%x drained", __func__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

void virtio_video_dec_cmd_resource_create_page(VirtIOVideoStream *stream,
    virtio_video_resource_create *req, virtio_video_mem_entry *entries,
    virtio_video_cmd_hdr *resp)
{
    VirtIOVideoResource *res;
    int i, j, dir;

    switch (req->queue_type) {
        case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
            dir = VIRTIO_VIDEO_RESOURCE_LIST_INPUT;
            break;
        case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
            dir = VIRTIO_VIDEO_RESOURCE_LIST_OUTPUT;
            break;
        default:
            return;
    }

    QLIST_FOREACH(res, &stream->resource_list[dir], next) {
        if (res->id == req->resource_id) {
            resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
            return;
        }
    }

    /* Frontend sometimes will not set planes layout, so do not return an error. */
    if (req->planes_layout != VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER &&
            req->planes_layout != VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE) {
        VIRTVID_WARN("    %s: stream 0x%x, create resource with invalid planes layout 0x%x",
                __func__, stream->id, req->planes_layout);
    }

    res = g_malloc0(sizeof(VirtIOVideoResource));
    res->id = req->resource_id;
    res->planes_layout = req->planes_layout;
    res->num_planes = req->num_planes;
    memcpy(&res->plane_offsets, &req->plane_offsets, sizeof(res->plane_offsets));
    memcpy(&res->num_entries, &req->num_entries, sizeof(res->num_entries));

    for (i = 0; i < res->num_planes; i++) {
        res->slices[i] = g_new0(VirtIOVideoResourceSlice, res->num_entries[i]);
        for (j = 0; j < res->num_entries[i]; j++) {
            MemoryRegionSection section = memory_region_find(get_system_memory(), entries->addr, entries->length);
            res->slices[i][j].page.hva = memory_region_get_ram_ptr(section.mr) + section.offset_within_region;
            res->slices[i][j].page.len = entries->length;
            entries++;
        }
    }

    qemu_mutex_lock(&stream->mutex);
    QLIST_INSERT_HEAD(&stream->resource_list[dir], res, next);
    qemu_mutex_unlock(&stream->mutex);
}

void virtio_video_dec_cmd_resource_create_object(VirtIOVideoStream *stream,
    virtio_video_resource_create *req, virtio_video_object_entry *entries,
    virtio_video_cmd_hdr *resp)
{
    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
    VIRTVID_ERROR("%s: Unsupported memory type (object)", __func__);
}

size_t virtio_video_dec_cmd_resource_queue(VirtIODevice *vdev,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
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
    VirtIOVideoStreamEventEntry *entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));

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
                qemu_event_set(&stream->signal_in);

                /* Wait for decode thread work done */
                qemu_event_wait(&stream->signal_out);
                qemu_event_reset(&stream->signal_out);
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

size_t virtio_video_dec_cmd_resource_destroy_all(VirtIODevice *vdev,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
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

size_t virtio_video_dec_cmd_queue_clear(VirtIODevice *vdev,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *stream, *next = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, next) {
        if (stream->id == req->hdr.stream_id) {
            VirtIOVideoStreamEventEntry *entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));

            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT || req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                entry->ev = VirtIOVideoStreamEventQueueClear;
                entry->data = g_malloc(sizeof(uint32_t));
                *(uint32_t*)(entry->data) = req->queue_type;

                qemu_mutex_lock(&stream->mutex);
                QLIST_INSERT_HEAD(&stream->ev_list, entry, next);
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_set(&stream->signal_in);
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

size_t virtio_video_dec_cmd_get_params(VirtIODevice *vdev,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *stream, *next = NULL;
    size_t len = 0;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, next) {
        if (stream->id == req->hdr.stream_id) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_PARAMS;
            if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                memcpy(&resp->params, &stream->in_params, sizeof(resp->params));
            } else if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                memcpy(&resp->params, &stream->out_params, sizeof(resp->params));
            } else {
                resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x",
                        __func__, req->hdr.stream_id, req->queue_type);
            }
            VIRTVID_DEBUG("    %s: stream 0x%x", __func__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_set_params(VirtIODevice *vdev,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *stream, *next = NULL;
    VirtIOVideoStreamMediaSDK *msdk;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, next) {
        if (stream->id == req->hdr.stream_id) {
            msdk = stream->opaque;
            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            if (req->params.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                memcpy(&stream->in_params, &req->params, sizeof(req->params));
            } else if (req->params.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                memcpy(&stream->out_params, &req->params, sizeof(req->params));
            } else {
                resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x",
                        __func__, req->hdr.stream_id, req->params.queue_type);
            }

            if (resp->type == VIRTIO_VIDEO_RESP_OK_NODATA) {
                VirtIOVideoStreamEventEntry *entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));

                entry->ev = VirtIOVideoStreamEventParamChange;
                qemu_mutex_lock(&stream->mutex);
                msdk->param.mfx.FrameInfo.Width = req->params.frame_width;
                msdk->param.mfx.FrameInfo.Height = req->params.frame_height;
                msdk->param.mfx.FrameInfo.FrameRateExtN = req->params.frame_rate;
                msdk->param.mfx.FrameInfo.FrameRateExtD = 1;
                QLIST_INSERT_HEAD(&stream->ev_list, entry, next);
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_set(&stream->signal_in);
            }

            VIRTVID_DEBUG("    %s: stream 0x%x", __func__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_query_control(VirtIODevice *vdev,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
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
        if (*resp == NULL)
            return 0;
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
        if (*resp == NULL)
            return 0;
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
    *resp = g_malloc0(sizeof(virtio_video_cmd_hdr));
    if (*resp == NULL)
        return 0;
    ((virtio_video_cmd_hdr *)(*resp))->type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
    ((virtio_video_cmd_hdr *)(*resp))->stream_id = req->hdr.stream_id;

    return sizeof(virtio_video_cmd_hdr);
}

size_t virtio_video_dec_cmd_get_control(VirtIODevice *vdev,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *stream, *next = NULL;
    size_t len = 0;

    switch (req->control) {
    case VIRTIO_VIDEO_CONTROL_BITRATE:
        len = sizeof(virtio_video_get_control_resp) + sizeof(virtio_video_control_val_bitrate);
        break;
    case VIRTIO_VIDEO_CONTROL_PROFILE:
        len = sizeof(virtio_video_get_control_resp) + sizeof(virtio_video_control_val_profile);
        break;
    case VIRTIO_VIDEO_CONTROL_LEVEL:
        len = sizeof(virtio_video_get_control_resp) + sizeof(virtio_video_control_val_level);
        break;
    default:
        len = sizeof(virtio_video_get_control_resp);
        VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __func__,
                req->hdr.stream_id, req->control);
        break;
    }

    *resp = g_malloc0(len);
    if (*resp != NULL) {
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        (*resp)->hdr.stream_id = req->hdr.stream_id;
        QLIST_FOREACH_SAFE(stream, &v->stream_list, next, next) {
            if (stream->id == req->hdr.stream_id) {
                (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;
                if (req->control == VIRTIO_VIDEO_CONTROL_BITRATE) {
                    ((virtio_video_control_val_bitrate*)((void*)(*resp) + sizeof(virtio_video_get_control_resp)))->bitrate = stream->control.bitrate;
                    VIRTVID_DEBUG("    %s: stream 0x%x bitrate %d", __func__, req->hdr.stream_id, stream->control.bitrate);
                } else if (req->control == VIRTIO_VIDEO_CONTROL_PROFILE) {
                    ((virtio_video_control_val_profile*)((void*)(*resp) + sizeof(virtio_video_get_control_resp)))->profile = stream->control.profile;
                    VIRTVID_DEBUG("    %s: stream 0x%x profile %d", __func__, req->hdr.stream_id, stream->control.profile);
                } else if (req->control == VIRTIO_VIDEO_CONTROL_LEVEL) {
                    ((virtio_video_control_val_level*)((void*)(*resp) + sizeof(virtio_video_get_control_resp)))->level = stream->control.level;
                    VIRTVID_DEBUG("    %s: stream 0x%x level %d", __func__, req->hdr.stream_id, stream->control.level);
                } else {
                    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
                    VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __func__, req->hdr.stream_id, req->control);
                }
                break;
            }
        }
    } else {
        len = 0;
    }

    return len;
}

size_t virtio_video_dec_cmd_set_control(VirtIODevice *vdev,
    virtio_video_set_control *req, virtio_video_set_control_resp *resp)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *stream, *next = NULL;
    VirtIOVideoStreamMediaSDK *msdk;
    size_t len = 0;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, next) {
        if (stream->id == req->hdr.stream_id) {
            msdk = stream->opaque;
            resp->hdr.type = VIRTIO_VIDEO_RESP_OK_NODATA;
            if (req->control == VIRTIO_VIDEO_CONTROL_BITRATE) {
                stream->control.bitrate = ((virtio_video_control_val_bitrate*)(((void*)req) + sizeof(virtio_video_set_control)))->bitrate;
                VIRTVID_DEBUG("    %s: stream 0x%x bitrate %d", __func__, req->hdr.stream_id, stream->control.bitrate);
            } else if (req->control == VIRTIO_VIDEO_CONTROL_PROFILE) {
                stream->control.profile = ((virtio_video_control_val_profile*)(((void*)req) + sizeof(virtio_video_set_control)))->profile;
                VIRTVID_DEBUG("    %s: stream 0x%x profile %d", __func__, req->hdr.stream_id, stream->control.profile);
            } else if (req->control == VIRTIO_VIDEO_CONTROL_LEVEL) {
                stream->control.level = ((virtio_video_control_val_level*)(((void*)req) + sizeof(virtio_video_set_control)))->level;
                VIRTVID_DEBUG("    %s: stream 0x%x level %d", __func__, req->hdr.stream_id, stream->control.level);
            } else {
                resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
                VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __func__, req->hdr.stream_id, req->control);
            }

            if (resp->hdr.type == VIRTIO_VIDEO_RESP_OK_NODATA) {
                VirtIOVideoStreamEventEntry *entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));

                entry->ev = VirtIOVideoStreamEventParamChange;
                qemu_mutex_lock(&stream->mutex);
                msdk->param.mfx.CodecProfile = virtio_video_profile_to_msdk(stream->control.profile);
                msdk->param.mfx.CodecLevel = virtio_video_level_to_msdk(stream->control.level);
                msdk->param.mfx.TargetKbps = stream->control.bitrate;
                QLIST_INSERT_HEAD(&stream->ev_list, entry, next);
                qemu_mutex_unlock(&stream->mutex);
                qemu_event_set(&stream->signal_in);
            }
            break;
        }
    }

    return len;
}

static int virtio_video_decode_init_msdk(VirtIODevice *vdev)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
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

    if (virtio_video_create_va_env_drm(vdev)) {
        VIRTVID_ERROR("Fail to create VA environment on DRM");
        return -1;
    }

    status = MFXInitEx(init_param, &session);
    if (status != MFX_ERR_NONE) {
        VIRTVID_ERROR("MFXInitEx returns %d", status);
        return -1;
    }

    status = MFXVideoCORE_SetHandle(session, MFX_HANDLE_VA_DISPLAY,
                                    ((VirtIOVideoMediaSDK *)v->opaque)->va_disp_handle);
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
        virtio_video_msdk_init_video_params(&param, in_format);
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

        in_fmt = g_malloc0(sizeof(VirtIOVideoFormat));
        virtio_video_msdk_init_format(in_fmt, in_format);

        in_fmt_frame = g_malloc0(sizeof(VirtIOVideoFormatFrame));
        in_fmt_frame->frame.width.min = w_min;
        in_fmt_frame->frame.width.max = w_max;
        in_fmt_frame->frame.width.step = VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
        in_fmt_frame->frame.height.min = h_min;
        in_fmt_frame->frame.height.max = h_max;
        in_fmt_frame->frame.height.step = (param.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
            VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHERS;

        /* For decoding, frame rate may be unspecified */
        in_fmt_frame->frame.num_rates = 1;
        in_fmt_frame->frame_rates = g_malloc0(sizeof(virtio_video_format_range));
        in_fmt_frame->frame_rates->min = 1;
        in_fmt_frame->frame_rates->max = 60;
        in_fmt_frame->frame_rates->step = 1;

        in_fmt->desc.num_frames++;
        QLIST_INSERT_HEAD(&in_fmt->frames, in_fmt_frame, next);
        QLIST_INSERT_HEAD(&v->format_list[VIRTIO_VIDEO_FORMAT_LIST_INPUT], in_fmt, next);

        VIRTVID_DEBUG("Add input caps for format %x, width [%d, %d]@%d, "
                      "height [%d, %d]@%d, rate [%d, %d]@%d", in_format,
                      w_min, w_max, in_fmt_frame->frame.width.step,
                      h_min, h_max, in_fmt_frame->frame.height.step,
                      in_fmt_frame->frame_rates->min, in_fmt_frame->frame_rates->max,
                      in_fmt_frame->frame_rates->step);

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

        out_fmt = g_malloc0(sizeof(VirtIOVideoFormat));
        virtio_video_msdk_init_format(out_fmt, out_format[i]);

        out_fmt_frame = g_malloc0(sizeof(VirtIOVideoFormatFrame));
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
                      out_fmt_frame->frame_rates->min, out_fmt_frame->frame_rates->max,
                      out_fmt_frame->frame_rates->step);
    }

    QLIST_FOREACH(in_fmt, &v->format_list[VIRTIO_VIDEO_FORMAT_LIST_INPUT], next) {
        for (i = 0; i < ARRAY_SIZE(out_format); i++) {
            in_fmt->desc.mask |= BIT_ULL(i);
        }
    }

    MFXClose(session);

    return 0;
}

static void virtio_video_decode_destroy_msdk(VirtIODevice *vdev)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *stream, *tmp_stream;

    QLIST_FOREACH_SAFE(stream, &v->stream_list, next, tmp_stream) {
        virtio_video_stream_destroy req = {0};
        virtio_video_cmd_hdr resp = {0};

        /* Destroy all in case CMD_STREAM_DESTROY not called on some stream */
        req.hdr.stream_id = stream->id;
        virtio_video_dec_cmd_stream_destroy(vdev, &req, &resp);
    }

    virtio_video_destroy_va_env_drm(vdev);
}

int virtio_video_decode_init(VirtIODevice *vdev)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    int ret = -1;

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        ret = virtio_video_decode_init_msdk(vdev);
    default:
        break;
    }

    VIRTVID_DEBUG("Decoder %s:%s initialized %d", v->conf.model, v->conf.backend, ret);

    return ret;
}

void virtio_video_decode_destroy(VirtIODevice *vdev)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);

    switch (v->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        virtio_video_decode_destroy_msdk(vdev);
    default:
        break;
    }
}
