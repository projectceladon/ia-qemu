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

size_t virtio_video_dec_cmd_get_params(VirtIODevice *vdev,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    size_t len = 0;

    VIRTVID_DEBUG("    %s: stream 0x%x, queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->queue_type);
    if (req != NULL && resp != NULL) {
        resp->hdr.type = req->hdr.type;
        resp->hdr.stream_id = req->hdr.stream_id;
        resp->params.queue_type = req->queue_type;
        len = sizeof(*resp);
    }

    return len;
}

size_t virtio_video_dec_event(VirtIODevice *vdev, virtio_video_event *ev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    size_t len = 0;

    if (ev) {
        ev->stream_id = ++vid->stream_id;
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

    vid->mfx_version_major = 1;
    vid->mfx_version_minor = 0;

    mfxInitParam par = {
        .Implementation = MFX_IMPL_AUTO_ANY,
        //.Implementation = MFX_IMPL_HARDWARE,
        .Version.Major = vid->mfx_version_major,
        .Version.Minor = vid->mfx_version_minor,
    };

    mfxVideoParam inParam = {0}, outParam = {0};

    if (virtio_video_create_va_env_drm(vdev)) {
        VIRTVID_ERROR("Fail to create VA environment on DRM\n");
        return -1;
    }

    sts = MFXInitEx(par, (mfxSession*)&vid->mfx_session);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("MFXInitEx returns %d", sts);
        return -1;
    }

    sts = MFXVideoCORE_SetHandle((mfxSession)vid->mfx_session, MFX_HANDLE_VA_DISPLAY, (mfxHDL)vid->va_disp_handle);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("MFXVideoCORE_SetHandle returns %d", sts);
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
            sts = MFXVideoDECODE_Query((mfxSession)vid->mfx_session, NULL, &outParam);
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

                virtio_video_msdk_load_plugin(vdev, coded_format, false, false);
                do {
                    sts = MFXVideoDECODE_Query((mfxSession)vid->mfx_session, &inParam, &outParam);
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
                    sts = MFXVideoDECODE_Query((mfxSession)vid->mfx_session, &inParam, &outParam);
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
                virtio_video_msdk_load_plugin(vdev, coded_format, false, true);

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

    return 0;
}

static void virtio_video_decode_destroy_msdk(VirtIODevice *vdev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);

    MFXClose((mfxSession)vid->mfx_session);
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