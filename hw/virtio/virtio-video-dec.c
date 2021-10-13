/*
 * VirtIO-Video Backend Driver
 * VirtIO-Video Backend Decoder
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
 * Author: Colin Xu <Colin.Xu@intel.com>
 *
 */
#include "qemu/osdep.h"
#include "virtio-video-dec.h"
#include "virtio-video-msdk.h"
#include "virtio-video-vaapi.h"

#define VIRTIO_VIDEO_DECODE_THREAD "Virtio-Video-Decode"

size_t virtio_video_dec_cmd_query_capability(VirtIODevice *vdev,
    virtio_video_query_capability *req, virtio_video_query_capability_resp **resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    size_t len = 0;
    void *src;
    VIRTVID_DEBUG("    %s: stream 0x%x, queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->queue_type);

    if (req != NULL && *resp == NULL) {
        switch (req->queue_type) {
        case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
            len = vid->caps_in.size;
            src = vid->caps_in.ptr;
            break;
        case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
            len = vid->caps_out.size;
            src = vid->caps_out.ptr;
            break;
        default:
            break;
        }

        *resp = g_malloc0(len);
        if (*resp != NULL) {
            memcpy(*resp, src, len);
            (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY;
            (*resp)->hdr.stream_id = req->hdr.stream_id;
        } else {
            len = 0;
        }
    }

    return len;
}

static void *virtio_video_decode_thread(void *arg)
{
    VirtIOVideoStream *stream = arg;
    sigset_t sigmask, old;
    int err, i;
    bool running = TRUE, decoding = TRUE;
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
        VIRTVID_ERROR("%s thread 0x%0x change SIG_BLOCK failed err %d", VIRTIO_VIDEO_DECODE_THREAD, stream->stream_id, err);
    }

    sts = MFXVideoDECODE_Init(stream->mfx_session, stream->mfxParams);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_Init failed with err %d", stream->stream_id, sts);
    }

    // Retrieve current working mfxVideoParam
    sts = MFXVideoDECODE_GetVideoParam(stream->mfx_session, stream->mfxParams);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_GetVideoParam failed with err %d", stream->stream_id, sts);
    }

    // Query and allocate working surface
    memset(&allocRequest, 0, sizeof(allocRequest));
    sts = MFXVideoDECODE_QueryIOSurf(stream->mfx_session, stream->mfxParams, &allocRequest);
    if (sts != MFX_ERR_NONE && sts != MFX_WRN_PARTIAL_ACCELERATION) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_QueryIOSurf failed with err %d", stream->stream_id, sts);
        running = FALSE;
    } else {
        mfxU16 width = (mfxU16) MSDK_ALIGN32(allocRequest.Info.Width);
        mfxU16 height = (mfxU16) MSDK_ALIGN32(allocRequest.Info.Height);
        mfxU8 bitsPerPixel = 12; // NV12 format is a 12 bits per pixel format
        mfxU32 surfaceSize = width * height * bitsPerPixel / 8;

        numSurfaces = allocRequest.NumFrameSuggested;
        surfaceBuffers = g_malloc0(surfaceSize * numSurfaces);
        surface_work = g_malloc0(numSurfaces * sizeof(mfxFrameSurface1));
        if (surfaceBuffers && surface_work) {
            for (i = 0; i < numSurfaces; i++) {
                surface_work[i].Info = ((mfxVideoParam*)stream->mfxParams)->mfx.FrameInfo;
                surface_work[i].Data.Y = &surfaceBuffers[surfaceSize * i];
                surface_work[i].Data.U = surface_work[i].Data.Y + width * height;
                surface_work[i].Data.V = surface_work[i].Data.U + 1;
                surface_work[i].Data.Pitch = width;
            }
        } else {
            VIRTVID_ERROR("stream 0x%x allocate working surface failed", stream->stream_id);
            running = FALSE;
        }
    }

    // Prepare VPP Params for color space conversion
    memset(&VPPParams, 0, sizeof(VPPParams));
    VPPParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    // Input
    VPPParams.vpp.In.FourCC = MFX_FOURCC_NV12;
    VPPParams.vpp.In.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    VPPParams.vpp.In.CropX = 0;
    VPPParams.vpp.In.CropY = 0;
    VPPParams.vpp.In.CropW = ((mfxVideoParam*)stream->mfxParams)->mfx.FrameInfo.CropW;
    VPPParams.vpp.In.CropH = ((mfxVideoParam*)stream->mfxParams)->mfx.FrameInfo.CropH;
    VPPParams.vpp.In.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    VPPParams.vpp.In.FrameRateExtN = ((mfxVideoParam*)stream->mfxParams)->mfx.FrameInfo.FrameRateExtN;
    VPPParams.vpp.In.FrameRateExtD = ((mfxVideoParam*)stream->mfxParams)->mfx.FrameInfo.FrameRateExtD;
    VPPParams.vpp.In.Height = (MFX_PICSTRUCT_PROGRESSIVE == VPPParams.vpp.In.PicStruct) ?
        MSDK_ALIGN16(((mfxVideoParam*)stream->mfxParams)->mfx.FrameInfo.Height) :
        MSDK_ALIGN32(((mfxVideoParam*)stream->mfxParams)->mfx.FrameInfo.Height) :
    VPPParams.vpp.In.Width = (MFX_PICSTRUCT_PROGRESSIVE == VPPParams.vpp.In.PicStruct) ?
        MSDK_ALIGN16(((mfxVideoParam*)stream->mfxParams)->mfx.FrameInfo.Width) :
        MSDK_ALIGN32(((mfxVideoParam*)stream->mfxParams)->mfx.FrameInfo.Width) :
    // Output
    memcpy(&VPPParams.vpp.Out, &VPPParams.vpp.In, sizeof(VPPParams.vpp.Out));
    VPPParams.vpp.Out.FourCC = virito_video_format_to_mfx4cc(stream->out_params.format);
    VPPParams.vpp.Out.ChromaFormat = 0;

    // Query and allocate VPP surface
    memset(&vppRequest, 0, sizeof(vppRequest));
    sts = MFXVideoVPP_QueryIOSurf(stream->mfx_session, &VPPParams, vppRequest);
    if (sts != MFX_ERR_NONE && sts != MFX_WRN_PARTIAL_ACCELERATION) {
        VIRTVID_ERROR("stream 0x%x MFXVideoVPP_QueryIOSurf failed with err %d", stream->stream_id, sts);
        running = FALSE;
    } else {
        ((mfxFrameSurface1*)stream->mfxSurfOut)->Info = VPPParams.vpp.Out;
        ((mfxFrameSurface1*)stream->mfxSurfOut)->Data.Pitch =
            ((mfxU16)MSDK_ALIGN32(vppRequest[1].Info.Width)) * 32 / 8;
    }

    sts = MFXVideoVPP_Init(stream->mfx_session, &VPPParams);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoVPP_Init failed with err %d", stream->stream_id, sts);
    }

    VIRTVID_DEBUG("%s thread 0x%0x running", VIRTIO_VIDEO_DECODE_THREAD, stream->stream_id);
    while (running) {
        mfxFrameSurface1 *surf = NULL;

        decoding = FALSE;

        qemu_event_wait(&stream->signal_in);
        qemu_event_reset(&stream->signal_in);

        qemu_mutex_lock(&stream->mutex);
        if (!QLIST_EMPTY(&stream->ev_list)) {
            VirtIOVideoStreamEventEntry *entry = QLIST_FIRST(&stream->ev_list);

            QLIST_SAFE_REMOVE(entry, next);

            switch (entry->ev) {
            case VirtIOVideoStreamEventParamChange:
                sts = MFXVideoDECODE_Reset(stream->mfx_session, stream->mfxParams);
                if (sts != MFX_ERR_NONE) {
                    running = FALSE;
                }
                break;
            case VirtIOVideoStreamEventStreamDrain:
                //set bs to NULL to signal end of stream to drain the decoding
                do {
                    sts = MFXVideoDECODE_DecodeFrameAsync(stream->mfx_session, NULL, surface_work, &surface_nv12, &syncp);
                    MFXVideoCORE_SyncOperation(stream->mfx_session, syncp, stream->mfxWaitMs);
                } while (sts != MFX_ERR_MORE_DATA && (--stream->retry) > 0);
                MFXVideoVPP_Reset(stream->mfx_session, &VPPParams);
                MFXVideoDECODE_Reset(stream->mfx_session, stream->mfxParams);
                break;
            case VirtIOVideoStreamEventResourceQueue:
                decoding = TRUE;
                break;
            case VirtIOVideoStreamEventQueueClear:
                decoding = FALSE;
                running = FALSE;
                // TODO: How to clear queue?
                if (*(uint32_t*)(entry->data) == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {

                } else { // VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT

                }
                break;
            case VirtIOVideoStreamEventTerminate:
                running = FALSE;
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

        sts = MFXVideoDECODE_DecodeFrameAsync(stream->mfx_session, stream->mfxBs, surf, &surface_nv12, &syncp);
        if (sts == MFX_ERR_NONE) {
            sts = MFXVideoCORE_SyncOperation(stream->mfx_session, syncp, stream->mfxWaitMs);
            if (sts == MFX_ERR_NONE) {
                stream->stat = VirtIOVideoStreamStatNone;
            } else {
                running = FALSE;
                stream->stat = VirtIOVideoStreamStatError;
                VIRTVID_ERROR("stream 0x%x MFXVideoCORE_SyncOperation failed with err %d", stream->stream_id, sts);
            }
        } else {
            running = FALSE;
            stream->stat = VirtIOVideoStreamStatError;
            VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_DecodeFrameAsync failed with err %d", stream->stream_id, sts);
        }

        for (;;) {
            sts = MFXVideoVPP_RunFrameVPPAsync(stream->mfx_session, surface_nv12, stream->mfxSurfOut, NULL, &syncVpp);
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

        // Notify CMD_RESOURCE_QUEUE, it's waiting for virtio_video_resource_queue_resp
        qemu_event_set(&stream->signal_out);
    }

    sts = MFXVideoVPP_Reset(stream->mfx_session, &VPPParams);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoVPP_Reset failed with err %d", stream->stream_id, sts);
    }

    sts = MFXVideoVPP_Close(stream->mfx_session);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoVPP_Close failed with err %d", stream->stream_id, sts);
    }

    sts = MFXVideoDECODE_Reset(stream->mfx_session, stream->mfxParams);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_Reset failed with err %d", stream->stream_id, sts);
    }

    sts = MFXVideoDECODE_Close(stream->mfx_session);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("stream 0x%x MFXVideoDECODE_Close failed with err %d", stream->stream_id, sts);
    }

    err = pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (err) {
        VIRTVID_ERROR("%s thread 0x%0x restore old sigmask failed err %d", VIRTIO_VIDEO_DECODE_THREAD, stream->stream_id, err);
    }

    g_free(surfaceBuffers);
    g_free(surface_work);

    VIRTVID_DEBUG("%s thread 0x%0x exits", VIRTIO_VIDEO_DECODE_THREAD, stream->stream_id);

    return NULL;
}

size_t virtio_video_dec_cmd_stream_create(VirtIODevice *vdev,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);
    if (virtio_video_msdk_find_format(&(vid->caps_in), req->coded_format, NULL)) {
        mfxStatus sts = MFX_ERR_NONE;
        VirtIOVideoStream *node = NULL;
        mfxInitParam par = {
            .Implementation = vid->mfx_impl,
            .Version.Major = vid->mfx_version_major,
            .Version.Minor = vid->mfx_version_minor,
        };

        node = g_malloc0(sizeof(VirtIOVideoStream));
        if (node) {
            VirtIOVideoControl *control = NULL;
            virtio_video_format_desc *desc = NULL;
            mfxVideoParam inParam = {0}, outParam = {0};
            int min, max;

            node->mfxWaitMs = 60000;
            node->stream_id = ~0L;

            sts = MFXInitEx(par, (mfxSession*)&node->mfx_session);
            if (sts != MFX_ERR_NONE) {
                VIRTVID_ERROR("    %s: MFXInitEx returns %d for stream 0x%x", __FUNCTION__, sts, req->hdr.stream_id);
                g_free(node);
                goto OUT;
            }

            sts = MFXVideoCORE_SetHandle(node->mfx_session, MFX_HANDLE_VA_DISPLAY, (mfxHDL)vid->va_disp_handle);
            if (sts != MFX_ERR_NONE) {
                VIRTVID_ERROR("    %s: MFXVideoCORE_SetHandle returns %d for stream 0x%x", __FUNCTION__, sts, req->hdr.stream_id);
                MFXClose(node->mfx_session);
                g_free(node);
                goto OUT;
            }

            virtio_video_msdk_load_plugin(vdev, node->mfx_session, req->coded_format, false, false);

            node->stream_id = req->hdr.stream_id;
            node->in_mem_type = req->in_mem_type;
            node->out_mem_type = req->out_mem_type;
            node->in_format = req->coded_format;
            memcpy(node->tag, req->tag, strlen((char*)req->tag));

            QLIST_INIT(&node->ev_list);

            // Prepare an initial mfxVideoParam for decode
            node->mfxParams = g_malloc0(sizeof(mfxVideoParam));
            virtio_video_msdk_fill_video_params(req->coded_format, node->mfxParams);

            node->mfxSurfOut = g_malloc(sizeof(mfxFrameSurface1));

            // TODO: Should we use VIDEO_MEMORY for virtio-gpu object?
            ((mfxVideoParam*)node->mfxParams)->IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

            // Try query all profiles
            QLIST_INIT(&node->control_caps.profile.list);
            virtio_video_msdk_fill_video_params(req->coded_format, &inParam);
            memset(&outParam, 0, sizeof(outParam));
            outParam.mfx.CodecId = inParam.mfx.CodecId;
            virtio_video_profile_range(req->coded_format, &min, &max);
            if (min != max) {
                int profile;
                for (profile = min; profile <= max; profile++) {
                    inParam.mfx.CodecProfile = virtio_video_profile_to_mfx(req->coded_format, profile);
                    if (inParam.mfx.CodecProfile != MFX_PROFILE_UNKNOWN) {
                        sts = MFXVideoDECODE_Query(node->mfx_session, &inParam, &outParam);
                        if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                            node->control.profile = profile;
                            control = g_malloc0(sizeof(VirtIOVideoControl));
                            if (control) {
                                node->control_caps.profile.num++;
                                control->value = profile;
                                QLIST_INSERT_HEAD(&node->control_caps.profile.list, control, next);
                            }
                        }
                    }
                }
                ((mfxVideoParam*)node->mfxParams)->mfx.CodecProfile = virtio_video_profile_to_mfx(req->coded_format, QLIST_FIRST(&node->control_caps.profile.list)->value);
            }

            // Try query all levels
            QLIST_INIT(&node->control_caps.level.list);
            virtio_video_msdk_fill_video_params(req->coded_format, &inParam);
            memset(&outParam, 0, sizeof(outParam));
            outParam.mfx.CodecId = inParam.mfx.CodecId;
            virtio_video_level_range(req->coded_format, &min, &max);
            if (min != max) {
                int level;
                for (level = min; level <= max; level++) {
                    inParam.mfx.CodecLevel = virtio_video_level_to_mfx(req->coded_format, level);
                    if (inParam.mfx.CodecLevel != MFX_LEVEL_UNKNOWN) {
                        sts = MFXVideoDECODE_Query(node->mfx_session, &inParam, &outParam);
                        if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                            node->control.level = level;
                            control = g_malloc0(sizeof(VirtIOVideoControl));
                            if (control) {
                                node->control_caps.level.num++;
                                control->value = level;
                                QLIST_INSERT_HEAD(&node->control_caps.level.list, control, next);
                            }
                        }
                    }
                }
                ((mfxVideoParam*)node->mfxParams)->mfx.CodecLevel = virtio_video_level_to_mfx(req->coded_format, QLIST_FIRST(&node->control_caps.level.list)->value);
            }

            virtio_video_msdk_fill_video_params(req->coded_format, &inParam);
            memset(&outParam, 0, sizeof(outParam));
            outParam.mfx.CodecId = inParam.mfx.CodecId;
            inParam.mfx.TargetKbps = 10000; //TODO: Determine the max bitrage
            sts = MFXVideoDECODE_Query(node->mfx_session, &inParam, &outParam);
            if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                node->control.bitrate = inParam.mfx.TargetKbps;
                ((mfxVideoParam*)node->mfxParams)->mfx.TargetKbps = node->control.bitrate;
            }

            memset(&node->in_params, 0, sizeof(node->in_params));

            if (virtio_video_msdk_find_format(&(vid->caps_in), node->in_format, &desc)) {
                node->in_params.frame_width = ((virtio_video_format_frame*)((void*)desc + sizeof(virtio_video_format_desc)))->width.max;
                node->in_params.frame_height = ((virtio_video_format_frame*)((void*)desc + sizeof(virtio_video_format_desc)))->height.max;
                node->in_params.min_buffers = 1;
                node->in_params.max_buffers = 1;
                node->in_params.crop.left = 0;
                node->in_params.crop.top = 0;
                node->in_params.crop.width = node->in_params.frame_width;
                node->in_params.crop.height = node->in_params.frame_height;
                node->in_params.frame_rate = ((virtio_video_format_range*)((void*)desc + sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame)))->max;

                memcpy(&node->out_params, &node->in_params, sizeof(node->in_params));

                // For VIRTIO_VIDEO_QUEUE_TYPE_INPUT
                node->in_params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
                node->in_params.format = node->in_format;
                // TODO: what's the definition of plane number, size and stride for coded format?
                node->in_params.num_planes = 1;
                node->in_params.plane_formats[0].plane_size = 0;
                node->in_params.plane_formats[0].stride = 0;

                // For VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT
                // Front end doesn't support NV12 but only RGB*, while MediaSDK can only decode to NV12
                // So we let front end aware of RGB* only, use VPP to convert from NV12 to RGB*
                node->out_params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
                node->out_params.format = VIRTIO_VIDEO_FORMAT_ARGB8888;
                node->out_params.num_planes = 1;
                node->out_params.plane_formats[0].plane_size = node->out_params.frame_width * node->out_params.frame_height * 4;
                node->out_params.plane_formats[0].stride = node->out_params.frame_width * 4;
            }

            ((mfxVideoParam*)node->mfxParams)->mfx.FrameInfo.Width = node->in_params.frame_width;
            ((mfxVideoParam*)node->mfxParams)->mfx.FrameInfo.Height = node->in_params.frame_height;
            ((mfxVideoParam*)node->mfxParams)->mfx.FrameInfo.FrameRateExtN = node->in_params.frame_rate;
            ((mfxVideoParam*)node->mfxParams)->mfx.FrameInfo.FrameRateExtD = 1;

            QLIST_INIT(&node->in_list);
            QLIST_INIT(&node->out_list);

            qemu_event_init(&node->signal_in, false);
            qemu_event_init(&node->signal_out, false);
            node->stat = VirtIOVideoStreamStatNone;

            qemu_mutex_init(&node->mutex);
            node->event_vq = vid->event_vq;

            qemu_thread_create(&node->thread, VIRTIO_VIDEO_DECODE_THREAD, virtio_video_decode_thread, node, QEMU_THREAD_JOINABLE);

            QLIST_INSERT_HEAD(&vid->stream_list, node, next);

            VIRTVID_DEBUG("    %s: stream 0x%x created", __FUNCTION__, req->hdr.stream_id);
            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
        }
    } else {
        VIRTVID_ERROR("    %s: stream 0x%x, unsupported format 0x%x", __FUNCTION__, req->hdr.stream_id, req->coded_format);
    }

OUT:
    return len;
}

size_t virtio_video_dec_cmd_stream_destroy(VirtIODevice *vdev,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    VirtIOVideoControl *control, *next_ctrl = NULL;
    VirtIOVideoStreamEventEntry *entry, *next_entry = NULL;
    VirtIOVideoStreamResource *res, *next_res = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;

            QLIST_FOREACH_SAFE(control, &node->control_caps.profile.list, next, next_ctrl) {
                QLIST_SAFE_REMOVE(control, next);
                g_free(control);
            }
            QLIST_FOREACH_SAFE(control, &node->control_caps.level.list, next, next_ctrl) {
                QLIST_SAFE_REMOVE(control, next);
                g_free(control);
            }

            entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));
            entry->ev = VirtIOVideoStreamEventTerminate;
            qemu_mutex_lock(&node->mutex);
            QLIST_INSERT_HEAD(&node->ev_list, entry, next);
            qemu_mutex_unlock(&node->mutex);
            qemu_event_set(&node->signal_in);

            // May need send SIGTERM if the thread is dead
            //pthread_kill(node->thread.thread, SIGTERM);
            qemu_thread_join(&node->thread);
            node->thread.thread = 0;

            QLIST_FOREACH_SAFE(entry, &node->ev_list, next, next_entry) {
                QLIST_SAFE_REMOVE(entry, next);
                g_free(entry);
            }

            QLIST_FOREACH_SAFE(res, &node->in_list, next, next_res) {
                QLIST_SAFE_REMOVE(res, next);
                g_free(res);
            }

            QLIST_FOREACH_SAFE(res, &node->out_list, next, next_res) {
                QLIST_SAFE_REMOVE(res, next);
                g_free(res);
            }

            qemu_event_destroy(&node->signal_in);
            qemu_event_destroy(&node->signal_out);

            qemu_mutex_destroy(&node->mutex);

            g_free(node->mfxSurfOut);
            g_free(node->mfxParams);
            virtio_video_msdk_load_plugin(vdev, node->mfx_session, node->in_format, false, true);
            MFXClose(node->mfx_session);

            QLIST_SAFE_REMOVE(node, next);
            g_free(node);
            VIRTVID_DEBUG("    %s: stream 0x%x destroyed", __FUNCTION__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_stream_drain(VirtIODevice *vdev,
    virtio_video_stream_drain *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            VirtIOVideoStreamEventEntry *entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));

            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            entry->ev = VirtIOVideoStreamEventStreamDrain;
            qemu_mutex_lock(&node->mutex);
            // Set retry count for drain
            node->retry = 10;
            QLIST_INSERT_HEAD(&node->ev_list, entry, next);
            qemu_mutex_unlock(&node->mutex);
            qemu_event_set(&node->signal_in);
            VIRTVID_DEBUG("    %s: stream 0x%x drained", __FUNCTION__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_resource_create(VirtIODevice *vdev,
    virtio_video_resource_create *req, virtio_video_mem_entry *entries,
    virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ||
                req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            } else {
                resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x",
                        __func__, req->hdr.stream_id, req->queue_type);
            }
            break;
        }
    }

    if (resp->type == VIRTIO_VIDEO_RESP_OK_NODATA) {
        VirtIOVideoStreamResource *res = g_malloc0(sizeof(VirtIOVideoStreamResource));
        uint32_t plane;

        res->resource_id = req->resource_id;
        res->mem_type = (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) ?
            node->in_mem_type : node->out_mem_type;
        res->planes_layout = req->planes_layout;
        res->num_planes = req->num_planes;
        memcpy(&res->plane_offsets, &req->plane_offsets, sizeof(res->plane_offsets));
        memcpy(&res->num_entries, &req->num_entries, sizeof(res->num_entries));

        for (plane = 0; plane < req->num_planes; plane++) {
            int i;

            res->desc[plane] = g_malloc(res->num_entries[plane] * sizeof(VirtIOVideoResourceDesc));
            for (i = 0; i < res->num_entries[plane]; i++) {
                if (res->mem_type == VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES) {
                    memcpy(&res->desc[plane][i].entry.mem_entry, entries++, sizeof(virtio_video_mem_entry));
                    virtio_video_resource_desc_from_guest_page(&res->desc[plane][i]);
                    memory_region_ref(res->desc[plane][i].mr);
                } else {
                    // TODO: Get from virtio object by uuid
                    memcpy(&res->desc[plane][i].entry.obj_entry, entries++, sizeof(virtio_video_object_entry));
                }
            }
        }

        qemu_mutex_lock(&node->mutex);
        if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
            QLIST_INSERT_HEAD(&node->in_list, res, next);
        } else {
            QLIST_INSERT_HEAD(&node->out_list, res, next);
        }
        qemu_mutex_unlock(&node->mutex);
    }

    return len;
}

size_t virtio_video_dec_cmd_resource_queue(VirtIODevice *vdev,
    virtio_video_resource_queue *req, virtio_video_resource_queue_resp *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);


    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            VirtIOVideoStreamResource *res, *next_res = NULL;
            VirtIOVideoStreamEventEntry *entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));

            if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
                QLIST_FOREACH_SAFE(res, &node->out_list, next, next_res) {
                    if (req->resource_id == res->resource_id) {
                        // Set mfxSurfOut buffer to the request hva, decode thread will fill other parameters
                        ((mfxFrameSurface1*)node->mfxSurfOut)->Data.Y = res->desc[0]->hva;
                        resp->hdr.type = VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE;
                    }
                }
            } else if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID;
                QLIST_FOREACH_SAFE(res, &node->in_list, next, next_res) {
                    if (req->resource_id == res->resource_id) {
                        // bitstream shouldn't have plane concept
                        ((mfxBitstream*)node->mfxBs)->MaxLength = req->data_sizes[0];
                        ((mfxBitstream*)node->mfxBs)->Data = res->desc[0]->hva;
                        resp->hdr.type = VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE;

                        // Notify decode thread to start on new input resource queued
                        entry->ev = VirtIOVideoStreamEventResourceQueue;
                        qemu_mutex_lock(&node->mutex);
                        QLIST_INSERT_HEAD(&node->ev_list, entry, next);
                        qemu_mutex_unlock(&node->mutex);
                        qemu_event_set(&node->signal_in);

                        // Wait for decode thread work done
                        qemu_event_wait(&node->signal_out);
                        qemu_event_reset(&node->signal_out);
                    }
                }
            } else {
                resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->queue_type);
            }

            resp->timestamp = req->timestamp;
            resp->size = 0; // Only for encode
            if (node->stat == VirtIOVideoStreamStatError) {
                resp->flags = VIRTIO_VIDEO_BUFFER_FLAG_ERR;
            }
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_resource_destroy_all(VirtIODevice *vdev,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    VirtIOVideoStreamResource *res, *next_res = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            uint32_t plane, desc;

            // TODO: Drain codec
            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                QLIST_FOREACH_SAFE(res, &node->in_list, next, next_res) {
                    for (plane = 0; plane < res->num_planes; plane++) {
                        for (desc = 0; desc < res->num_entries[plane]; desc++) {
                            memory_region_unref(res->desc[plane][desc].mr);
                        }
                        g_free(res->desc[plane]);
                    }
                    QLIST_SAFE_REMOVE(res, next);
                    g_free(res);
                }
            } else if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                QLIST_FOREACH_SAFE(res, &node->out_list, next, next_res) {
                    for (plane = 0; plane < res->num_planes; plane++) {
                        g_free(res->desc[plane]);
                    }
                    QLIST_SAFE_REMOVE(res, next);
                    g_free(res);
                }
            } else {
                resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->queue_type);
            }
            VIRTVID_DEBUG("    %s: stream 0x%x queue_type 0x%x all resource destroyed", __FUNCTION__, req->hdr.stream_id, req->queue_type);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_queue_clear(VirtIODevice *vdev,
    virtio_video_queue_clear *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            VirtIOVideoStreamEventEntry *entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));

            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT || req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                entry->ev = VirtIOVideoStreamEventQueueClear;
                entry->data = g_malloc(sizeof(uint32_t));
                *(uint32_t*)(entry->data) = req->queue_type;

                qemu_mutex_lock(&node->mutex);
                QLIST_INSERT_HEAD(&node->ev_list, entry, next);
                qemu_mutex_unlock(&node->mutex);
                qemu_event_set(&node->signal_in);
            } else {
                resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                g_free(entry);
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->queue_type);
            }

            VIRTVID_DEBUG("    %s: stream 0x%x queue_type 0x%x cleared", __FUNCTION__, req->hdr.stream_id, req->queue_type);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_get_params(VirtIODevice *vdev,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_PARAMS;
            if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                memcpy(&resp->params, &node->in_params, sizeof(resp->params));
            } else if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                memcpy(&resp->params, &node->out_params, sizeof(resp->params));
            } else {
                resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->queue_type);
            }
            VIRTVID_DEBUG("    %s: stream 0x%x", __FUNCTION__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_set_params(VirtIODevice *vdev,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            if (req->params.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                memcpy(&node->in_params, &req->params, sizeof(req->params));
            } else if (req->params.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                memcpy(&node->out_params, &req->params, sizeof(req->params));
            } else {
                resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->params.queue_type);
            }

            if (resp->type == VIRTIO_VIDEO_RESP_OK_NODATA) {
                VirtIOVideoStreamEventEntry *entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));

                entry->ev = VirtIOVideoStreamEventParamChange;
                qemu_mutex_lock(&node->mutex);
                ((mfxVideoParam*)node->mfxParams)->mfx.FrameInfo.Width = req->params.frame_width;
                ((mfxVideoParam*)node->mfxParams)->mfx.FrameInfo.Height = req->params.frame_height;
                ((mfxVideoParam*)node->mfxParams)->mfx.FrameInfo.FrameRateExtN = req->params.frame_rate;
                ((mfxVideoParam*)node->mfxParams)->mfx.FrameInfo.FrameRateExtD = 1;
                QLIST_INSERT_HEAD(&node->ev_list, entry, next);
                qemu_mutex_unlock(&node->mutex);
                qemu_event_set(&node->signal_in);
            }

            VIRTVID_DEBUG("    %s: stream 0x%x", __FUNCTION__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_query_control(VirtIODevice *vdev,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    *resp = NULL;
    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            if (req->control == VIRTIO_VIDEO_CONTROL_PROFILE) {
                virtio_video_format format = ((virtio_video_query_control_profile*)((void*)req + sizeof(virtio_video_query_control)))->format;

                if (format == node->in_format) {
                    len = sizeof(virtio_video_query_control_resp);
                    len += sizeof(virtio_video_query_control_resp_profile);
                    len += node->control_caps.profile.num * sizeof(__le32);
                } else {
                    VIRTVID_ERROR("    %s: stream 0x%x format %d mismatch requested %d", __FUNCTION__, req->hdr.stream_id, node->in_format, format);
                }

                *resp = g_malloc0(len);
                if (*resp != NULL) {
                    VirtIOVideoControl *control, *next_ctrl = NULL;
                    __le32 *profile = (void*)(*resp) + sizeof(virtio_video_query_control_resp) + sizeof(virtio_video_query_control_resp_profile);

                    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL;
                    QLIST_FOREACH_SAFE(control, &node->control_caps.profile.list, next, next_ctrl) {
                        *profile = control->value;
                        ((virtio_video_query_control_resp_profile*)((void*)(*resp) + sizeof(virtio_video_query_control_resp)))->num++;
                        profile++;
                    }
                } else {
                    len = 0;
                }
                VIRTVID_DEBUG("    %s: stream 0x%x support %d profiles", __FUNCTION__, req->hdr.stream_id, node->control_caps.profile.num);
            } else if (req->control == VIRTIO_VIDEO_CONTROL_LEVEL) {
                virtio_video_format format = ((virtio_video_query_control_level*)((void*)req + sizeof(virtio_video_query_control)))->format;

                if (format == node->in_format) {
                    len = sizeof(virtio_video_query_control_resp);
                    len += sizeof(virtio_video_query_control_resp_level);
                    len += node->control_caps.level.num * sizeof(__le32);
                } else {
                    VIRTVID_ERROR("    %s: stream 0x%x format %d mismatch requested %d", __FUNCTION__, req->hdr.stream_id, node->in_format, format);
                }

                *resp = g_malloc0(len);
                if (*resp != NULL) {
                    VirtIOVideoControl *control, *next_ctrl = NULL;
                    __le32 *level = (void*)(*resp) + sizeof(virtio_video_query_control_resp) + sizeof(virtio_video_query_control_resp_level);

                    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL;
                    QLIST_FOREACH_SAFE(control, &node->control_caps.level.list, next, next_ctrl) {
                        *level = control->value;
                        ((virtio_video_query_control_resp_level*)((void*)(*resp) + sizeof(virtio_video_query_control_resp)))->num++;
                        level++;
                    }
                } else {
                    len = 0;
                }
                VIRTVID_DEBUG("    %s: stream 0x%x support %d levels", __FUNCTION__, req->hdr.stream_id, node->control_caps.level.num);
            } else {
                len = sizeof(virtio_video_query_control_resp);
                *resp = g_malloc0(len);
                if (*resp != NULL) {
                    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
                    (*resp)->hdr.stream_id = req->hdr.stream_id;
                } else {
                    len = 0;
                }
                VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __FUNCTION__, req->hdr.stream_id, req->control);
            }
            break;
        }
    }

    if (*resp == NULL) {
        len = sizeof(virtio_video_query_control_resp);
        *resp = g_malloc0(len);
        if (*resp != NULL) {
            (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
            (*resp)->hdr.stream_id = req->hdr.stream_id;
        } else {
            len = 0;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_get_control(VirtIODevice *vdev,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
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
        VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __FUNCTION__, req->hdr.stream_id, req->control);
        break;
    }

    *resp = g_malloc0(len);
    if (*resp != NULL) {
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        (*resp)->hdr.stream_id = req->hdr.stream_id;
        QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
            if (node->stream_id == req->hdr.stream_id) {
                (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;
                if (req->control == VIRTIO_VIDEO_CONTROL_BITRATE) {
                    ((virtio_video_control_val_bitrate*)((void*)(*resp) + sizeof(virtio_video_get_control_resp)))->bitrate = node->control.bitrate;
                    VIRTVID_DEBUG("    %s: stream 0x%x bitrate %d", __FUNCTION__, req->hdr.stream_id, node->control.bitrate);
                } else if (req->control == VIRTIO_VIDEO_CONTROL_PROFILE) {
                    ((virtio_video_control_val_profile*)((void*)(*resp) + sizeof(virtio_video_get_control_resp)))->profile = node->control.profile;
                    VIRTVID_DEBUG("    %s: stream 0x%x profile %d", __FUNCTION__, req->hdr.stream_id, node->control.profile);
                } else if (req->control == VIRTIO_VIDEO_CONTROL_LEVEL) {
                    ((virtio_video_control_val_level*)((void*)(*resp) + sizeof(virtio_video_get_control_resp)))->level = node->control.level;
                    VIRTVID_DEBUG("    %s: stream 0x%x level %d", __FUNCTION__, req->hdr.stream_id, node->control.level);
                } else {
                    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
                    VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __FUNCTION__, req->hdr.stream_id, req->control);
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
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_OK_NODATA;
            if (req->control == VIRTIO_VIDEO_CONTROL_BITRATE) {
                node->control.bitrate = ((virtio_video_control_val_bitrate*)(((void*)req) + sizeof(virtio_video_set_control)))->bitrate;
                VIRTVID_DEBUG("    %s: stream 0x%x bitrate %d", __FUNCTION__, req->hdr.stream_id, node->control.bitrate);
            } else if (req->control == VIRTIO_VIDEO_CONTROL_PROFILE) {
                node->control.profile = ((virtio_video_control_val_profile*)(((void*)req) + sizeof(virtio_video_set_control)))->profile;
                VIRTVID_DEBUG("    %s: stream 0x%x profile %d", __FUNCTION__, req->hdr.stream_id, node->control.profile);
            } else if (req->control == VIRTIO_VIDEO_CONTROL_LEVEL) {
                node->control.level = ((virtio_video_control_val_level*)(((void*)req) + sizeof(virtio_video_set_control)))->level;
                VIRTVID_DEBUG("    %s: stream 0x%x level %d", __FUNCTION__, req->hdr.stream_id, node->control.level);
            } else {
                resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
                VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __FUNCTION__, req->hdr.stream_id, req->control);
            }

            if (resp->hdr.type == VIRTIO_VIDEO_RESP_OK_NODATA) {
                VirtIOVideoStreamEventEntry *entry = g_malloc0(sizeof(VirtIOVideoStreamEventEntry));

                entry->ev = VirtIOVideoStreamEventParamChange;
                qemu_mutex_lock(&node->mutex);
                ((mfxVideoParam*)node->mfxParams)->mfx.CodecProfile = virtio_video_profile_to_mfx(node->in_format, node->control.profile);
                ((mfxVideoParam*)node->mfxParams)->mfx.CodecLevel = virtio_video_level_to_mfx(node->in_format, node->control.level);
                ((mfxVideoParam*)node->mfxParams)->mfx.TargetKbps = node->control.bitrate;
                QLIST_INSERT_HEAD(&node->ev_list, entry, next);
                qemu_mutex_unlock(&node->mutex);
                qemu_event_set(&node->signal_in);
            }
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_event(VirtIODevice *vdev, virtio_video_event *ev)
{
    //VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    size_t len = 0;

    if (ev) {
        //ev->stream_id = ++vid->stream_id;
        len = sizeof(*ev);
        VIRTVID_DEBUG("    %s: event_type 0x%x, stream_id 0x%x", __FUNCTION__, ev->event_type, ev->stream_id);
    } else {
        VIRTVID_ERROR("Invalid virtio_video_event buffer");
    }

    return len;
}

static int virtio_video_decode_init_msdk(VirtIODevice *vdev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    mfxStatus sts = MFX_ERR_NONE;
    virtio_video_format coded_format;
    mfxSession mfx_session;

    vid->mfx_impl = MFX_IMPL_AUTO_ANY;//MFX_IMPL_HARDWARE
    vid->mfx_version_major = 1;
    vid->mfx_version_minor = 0;

    mfxInitParam par = {
        .Implementation = vid->mfx_impl,
        .Version.Major = vid->mfx_version_major,
        .Version.Minor = vid->mfx_version_minor,
    };

    mfxVideoParam inParam = {0}, outParam = {0};

    if (virtio_video_create_va_env_drm(vdev)) {
        VIRTVID_ERROR("Fail to create VA environment on DRM");
        return -1;
    }

    sts = MFXInitEx(par, &mfx_session);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("MFXInitEx returns %d", sts);
        return -1;
    }

    sts = MFXVideoCORE_SetHandle(mfx_session, MFX_HANDLE_VA_DISPLAY, (mfxHDL)vid->va_disp_handle);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("MFXVideoCORE_SetHandle returns %d", sts);
        MFXClose(mfx_session);
        return -1;
    }

    for (coded_format = VIRTIO_VIDEO_FORMAT_CODED_MIN;
         coded_format <= VIRTIO_VIDEO_FORMAT_CODED_MAX;
         coded_format++) {
        int coded_mfx4cc = virito_video_format_to_mfx4cc(coded_format);
        if (coded_mfx4cc != 0) {
            // Query CodecId to fill virtio_video_format_desc
            memset(&outParam, 0, sizeof(outParam));
            outParam.mfx.CodecId = coded_mfx4cc;
            sts = MFXVideoDECODE_Query(mfx_session, NULL, &outParam);
            if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                void *buf = NULL;
                uint32_t w_min = 0, h_min = 0, w_max = 0, h_max = 0;

                // Add a new virtio_video_format_desc block since current format is supported
                ++((virtio_video_query_capability_resp*)vid->caps_in.ptr)->num_descs;

                // Save old caps, allocate a larger buffer, copy it back
                buf = g_malloc0(vid->caps_in.size);
                memcpy(buf, vid->caps_in.ptr, vid->caps_in.size);
                g_free(vid->caps_in.ptr);
                vid->caps_in.size += sizeof(virtio_video_format_desc);
                vid->caps_in.ptr = g_malloc0(vid->caps_in.size);
                memcpy(vid->caps_in.ptr, buf, vid->caps_in.size - sizeof(virtio_video_format_desc));
                g_free(buf);

                // Append the newly added virtio_video_format_desc
                buf = (char*)vid->caps_in.ptr + vid->caps_in.size - sizeof(virtio_video_format_desc);
                virtio_video_msdk_fill_format_desc(coded_format, (virtio_video_format_desc*)buf);

                // Try query max & min size for a coded format
                virtio_video_msdk_fill_video_params(coded_format, &inParam);
                memset(&outParam, 0, sizeof(outParam));
                outParam.mfx.CodecId = inParam.mfx.CodecId;

                inParam.mfx.FrameInfo.Width = VIRTIO_VIDEO_MSDK_DIMENSION_MAX;
                inParam.mfx.FrameInfo.Height = VIRTIO_VIDEO_MSDK_DIMENSION_MAX;

                virtio_video_msdk_load_plugin(vdev, mfx_session, coded_format, false, false);
                do {
                    sts = MFXVideoDECODE_Query(mfx_session, &inParam, &outParam);
                    if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                        w_max = outParam.mfx.FrameInfo.Width;
                        h_max = outParam.mfx.FrameInfo.Height;
                        break;
                    }
                    inParam.mfx.FrameInfo.Width -= VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
                    if (inParam.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) {
                        inParam.mfx.FrameInfo.Height -= VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
                    } else {
                        inParam.mfx.FrameInfo.Height -= VIRTIO_VIDEO_MSDK_DIM_STEP_OTHER;
                    }
                } while (inParam.mfx.FrameInfo.Width >= VIRTIO_VIDEO_MSDK_DIMENSION_MIN && inParam.mfx.FrameInfo.Height >= VIRTIO_VIDEO_MSDK_DIMENSION_MIN);

                inParam.mfx.FrameInfo.Width = VIRTIO_VIDEO_MSDK_DIMENSION_MIN;
                inParam.mfx.FrameInfo.Height = VIRTIO_VIDEO_MSDK_DIMENSION_MIN;
                do {
                    sts = MFXVideoDECODE_Query(mfx_session, &inParam, &outParam);
                    if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                        w_min = outParam.mfx.FrameInfo.Width;
                        h_min = outParam.mfx.FrameInfo.Height;
                        break;
                    }
                    inParam.mfx.FrameInfo.Width += VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
                    if (inParam.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) {
                        inParam.mfx.FrameInfo.Height += VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
                    } else {
                        inParam.mfx.FrameInfo.Height += VIRTIO_VIDEO_MSDK_DIM_STEP_OTHER;
                    }
                } while (inParam.mfx.FrameInfo.Width <= w_max && inParam.mfx.FrameInfo.Height <= h_max);
                virtio_video_msdk_load_plugin(vdev, mfx_session, coded_format, false, true);

                // Add one virtio_video_format_frame and virtio_video_format_range block to last added virtio_video_format_desc
                if (w_min && w_max && h_min && h_max) {
                    void *buf_out = NULL;
                    uint32_t pos = 0;
                    uint32_t desc_size = 0;

                    buf = (char*)vid->caps_in.ptr + vid->caps_in.size - sizeof(virtio_video_format_desc);
                    ((virtio_video_format_desc*)buf)->num_frames = 1;

                    // Save old caps, allocate a larger buffer, copy it back
                    buf = g_malloc0(vid->caps_in.size);
                    memcpy(buf, vid->caps_in.ptr, vid->caps_in.size);
                    g_free(vid->caps_in.ptr);
                    vid->caps_in.size += sizeof(virtio_video_format_frame);
                    vid->caps_in.size += sizeof(virtio_video_format_range);
                    vid->caps_in.ptr = g_malloc0(vid->caps_in.size);
                    memcpy(vid->caps_in.ptr, buf,
                           vid->caps_in.size - sizeof(virtio_video_format_frame) - sizeof(virtio_video_format_range));
                    g_free(buf);

                    // Append the newly added virtio_video_format_frame and virtio_video_format_range
                    buf = (char*)vid->caps_in.ptr + vid->caps_in.size;
                    buf -= sizeof(virtio_video_format_frame);
                    buf -= sizeof(virtio_video_format_range);
                    ((virtio_video_format_frame*)buf)->width.min = w_min;
                    ((virtio_video_format_frame*)buf)->width.max = w_max;
                    ((virtio_video_format_frame*)buf)->width.step = VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
                    ((virtio_video_format_frame*)buf)->height.min = h_min;
                    ((virtio_video_format_frame*)buf)->height.max = h_max;
                    ((virtio_video_format_frame*)buf)->height.step =
                        (inParam.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
                        VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHER;
                    ((virtio_video_format_frame*)buf)->num_rates = 1;

                    buf += sizeof(virtio_video_format_frame);
                    // For decoding, frame rate may be unspecified, so always set range [1,60]
                    ((virtio_video_format_range*)buf)->min = 1;
                    ((virtio_video_format_range*)buf)->max = 60;
                    ((virtio_video_format_range*)buf)->step = 1;

                    VIRTVID_DEBUG("Add input caps for format %x, width [%d, %d]@%d, height [%d, %d]@%d, rate [%d, %d]@%d",
                                  coded_format,
                                  w_min, w_max, VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE,
                                  h_min, h_max, (inParam.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ? VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHER,
                                  ((virtio_video_format_range*)buf)->min,
                                  ((virtio_video_format_range*)buf)->max,
                                  ((virtio_video_format_range*)buf)->step);

                    // Allocate a new block for output cap, copy format_frame format_desc from latest input cap
                    desc_size = sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame) + sizeof(virtio_video_format_range);
                    buf_out = g_malloc0(desc_size);
                    virtio_video_msdk_fill_format_desc(VIRTIO_VIDEO_FORMAT_NV12, (virtio_video_format_desc*)buf_out);
                    ((virtio_video_format_desc*)buf_out)->num_frames = 1;
                    buf -= sizeof(virtio_video_format_frame);
                    memcpy(buf_out + sizeof(virtio_video_format_desc), buf, sizeof(virtio_video_format_frame) + sizeof(virtio_video_format_range));

                    // Check if caps_out already have the format, add if not exist
                    if (!virtio_video_msdk_find_format_desc(&(vid->caps_out), (virtio_video_format_desc*)buf_out)) {
                        pos = vid->caps_out.size;

                        ++((virtio_video_query_capability_resp*)vid->caps_out.ptr)->num_descs;
                        // Save old caps, allocate a larger buffer, copy it back
                        buf = g_malloc0(vid->caps_out.size);
                        memcpy(buf, vid->caps_out.ptr, vid->caps_out.size);
                        g_free(vid->caps_out.ptr);
                        vid->caps_out.size += desc_size;
                        vid->caps_out.ptr = g_malloc0(vid->caps_out.size);
                        memcpy(vid->caps_out.ptr, buf, pos);
                        g_free(buf);

                        // Append the newly added virtio_video_format_desc, virtio_video_format_frame and virtio_video_format_range
                        memcpy(vid->caps_out.ptr + pos, buf_out, desc_size);

                        VIRTVID_DEBUG("Add output caps for format %x, width [%d, %d]@%d, height [%d, %d]@%d, rate [%d, %d]@%d",
                                      VIRTIO_VIDEO_FORMAT_NV12,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->width.min,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->width.max,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->width.step,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->height.min,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->height.max,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->height.step,
                                      ((virtio_video_format_range*)(buf_out + sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame)))->min,
                                      ((virtio_video_format_range*)(buf_out + sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame)))->max,
                                      ((virtio_video_format_range*)(buf_out + sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame)))->step
                                      );
                    }
                    g_free(buf_out);
                }
            } else {
                VIRTVID_DEBUG("format %x isn't supported by MSDK, status %d", coded_format, sts);
            }
        }
    }

    MFXClose(mfx_session);

    return 0;
}

static void virtio_video_decode_destroy_msdk(VirtIODevice *vdev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        virtio_video_stream_destroy req = {0};
        virtio_video_cmd_hdr resp = {0};

        // Destroy all in case CMD_STREAM_DESTROY not called on some stream
        req.hdr.stream_id = node->stream_id;
        virtio_video_dec_cmd_stream_destroy(vdev, &req, &resp);
    }

    virtio_video_destroy_va_env_drm(vdev);
}

int virtio_video_decode_init(VirtIODevice *vdev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    int ret = -1;

    switch (vid->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        ret = virtio_video_decode_init_msdk(vdev);
    default:
        break;
    }

    VIRTVID_DEBUG("Decoder %s:%s initialized %d", vid->property.model, vid->property.backend, ret);

    return ret;
}

void virtio_video_decode_destroy(VirtIODevice *vdev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);

    switch (vid->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        virtio_video_decode_destroy_msdk(vdev);
    default:
        break;
    }
}
