/*
 * VirtIO-Video Backend Driver
 * VirtIO-Video Backend MediaSDK
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
#include "virtio-video-vaapi.h"
#include "virtio-video-msdk.h"

void virtio_video_msdk_fill_video_params(virtio_video_format format, mfxVideoParam *param)
{
    if (param == NULL) {
        return;
    }

    if (virito_video_format_to_mfx4cc(format) == 0) {
        return;
    }

    memset(param, 0, sizeof(*param));
    param->mfx.CodecId = virito_video_format_to_mfx4cc(format);
    param->mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

    switch (format) {
    case VIRTIO_VIDEO_FORMAT_ARGB8888:
    case VIRTIO_VIDEO_FORMAT_BGRA8888:
    case VIRTIO_VIDEO_FORMAT_NV12:
    case VIRTIO_VIDEO_FORMAT_YUV420:
    case VIRTIO_VIDEO_FORMAT_YVU420:
        break;
    case VIRTIO_VIDEO_FORMAT_MPEG2:
    case VIRTIO_VIDEO_FORMAT_MPEG4:
    case VIRTIO_VIDEO_FORMAT_H264:
    case VIRTIO_VIDEO_FORMAT_HEVC:
    case VIRTIO_VIDEO_FORMAT_VP8:
    case VIRTIO_VIDEO_FORMAT_VP9:
        // Only support NV12 for now
        param->mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
        param->mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        if (format == VIRTIO_VIDEO_FORMAT_HEVC) {
            param->mfx.CodecProfile = MFX_PROFILE_HEVC_MAIN;
        }
        break;
    default:
        break;
    }
}

void virtio_video_msdk_fill_format_desc(virtio_video_format format, virtio_video_format_desc *format_desc)
{
    if (format_desc == NULL) {
        return;
    }

    memset(format_desc, 0, sizeof(*format_desc));
    format_desc->mask = 0;
    format_desc->format = format;
    format_desc->planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER;
    format_desc->plane_align = 0;
}

bool virtio_video_msdk_find_format_desc(VirtIOVideoCaps *caps, virtio_video_format_desc *format_desc)
{
    bool found = false;
    uint32_t idx_format, idx_frame, idx_rate, num_format, num_frame, num_rate;
    void *src1, *src2;

    // Assume the format_desc only have 1 format_desc, 1 format_frame and 1 format_range
    if (format_desc->num_frames > 1) {
        VIRTVID_ERROR("Not support finding virtio_video_format_desc with num_frames %d", format_desc->num_frames);
        return false;
    }
    if (format_desc->num_frames == 1) {
        src1 = (void*)format_desc + sizeof(virtio_video_format_desc);
        if (((virtio_video_format_frame*)src1)->num_rates > 1) {
            VIRTVID_ERROR("Not support finding virtio_video_format_desc with num_rates %d", ((virtio_video_format_frame*)src1)->num_rates);
            return false;
        }
    }

    if (caps != NULL && format_desc != NULL){
        num_format = ((virtio_video_query_capability_resp*)caps->ptr)->num_descs;
        src1 = caps->ptr + sizeof(virtio_video_query_capability_resp);
        for (idx_format = 0; idx_format < num_format; idx_format++) {
            // Compare the virtio_video_format_desc part
            found = false;
            src2 = format_desc;
            num_frame = ((virtio_video_format_desc*)src1)->num_frames;
            if (memcmp(src1, src2, sizeof(virtio_video_format_desc)) == 0) {
                VIRTVID_VERBOSE("format_desc equal at %d", idx_format);
                if (num_frame == 0) {
                    // Same format_desc but no frame
                    found = true;
                    idx_format = num_format;
                } else {
                    // Advance to compare format_frame
                    src1 += sizeof(virtio_video_format_desc);
                    for (idx_frame = 0; idx_frame < num_frame; idx_frame++) {
                        // Compare the virtio_video_format_frame part
                        src2 = (void*)format_desc + sizeof(virtio_video_format_desc);
                        num_rate = ((virtio_video_format_frame*)src1)->num_rates;
                        if (memcmp(src1, src2, sizeof(virtio_video_format_frame)) == 0) {
                            VIRTVID_VERBOSE("format_frame equal at %d-%d", idx_format, idx_frame);
                            if (num_rate == 0) {
                                // Same format_frame but no rate
                                found = true;
                                idx_frame = num_frame;
                                idx_format = num_format;
                            } else {
                                // Advance to compare format_range
                                src1 += sizeof(virtio_video_format_frame);
                                for (idx_rate = 0; idx_rate < num_rate; idx_rate++) {
                                    // Compare the virtio_video_format_range part
                                    src2 = (void*)format_desc + sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame);
                                    if (memcmp(src1, src2, sizeof(virtio_video_format_range)) == 0) {
                                        VIRTVID_VERBOSE("format_range equal at %d-%d-%d", idx_format, idx_frame, idx_rate);
                                        // Same format_range, format_frame format_desc
                                        found = true;
                                        idx_rate = num_rate;
                                        idx_frame = num_frame;
                                        idx_format = num_format;
                                    } else {
                                        VIRTVID_VERBOSE("format_range NOT equal at %d-%d-%d", idx_format, idx_frame, idx_rate);
                                    }
                                    // Advance to next format_range
                                    src1 += sizeof(virtio_video_format_range);
                                }
                            }
                        } else {
                            VIRTVID_VERBOSE("format_frame NOT equal at %d-%d", idx_format, idx_frame);
                            // Skip current format_frame
                            src1 += sizeof(virtio_video_format_frame);
                            if (num_rate != 0) {
                                // Skip remaining format_range
                                for (idx_rate = 0; idx_rate < num_rate; idx_rate++) {
                                    src1 += sizeof(virtio_video_format_range);
                                }
                            }
                        }
                    }
                }
            } else {
                VIRTVID_VERBOSE("format_desc NOT equal at %d", idx_format);
                // Skip current format_desc
                src1 += sizeof(virtio_video_format_desc);
                if (num_frame != 0) {
                    // Skip remaining format_frame
                    for (idx_frame = 0; idx_frame < num_frame; idx_frame++) {
                        num_rate = ((virtio_video_format_frame*)src1)->num_rates;
                        src1 += sizeof(virtio_video_format_frame);
                        if (num_rate != 0) {
                            for (idx_rate = 0; idx_rate < num_rate; idx_rate++) {
                                src1 += sizeof(virtio_video_format_range);
                            }
                        }
                    }
                }
            }
        }
    }

    return found;
}

int virtio_video_msdk_get_plugin(virtio_video_format format, bool encode, mfxPluginUID *plugin)
{
    int ret = -1;

    if (plugin != NULL) {
        if (encode) {
            switch (format) {
            case VIRTIO_VIDEO_FORMAT_HEVC:
                *plugin = MFX_PLUGINID_HEVCE_HW;
                ret = 0;
                break;
            case VIRTIO_VIDEO_FORMAT_VP9:
                *plugin = MFX_PLUGINID_VP9E_HW;
                ret = 0;
                break;
            default:
                break;
            }
        } else {
            switch (format) {
            case VIRTIO_VIDEO_FORMAT_HEVC:
                *plugin = MFX_PLUGINID_HEVCD_HW;
                ret = 0;
                break;
            case VIRTIO_VIDEO_FORMAT_VP8:
                *plugin = MFX_PLUGINID_VP8D_HW;
                ret = 0;
                break;
            case VIRTIO_VIDEO_FORMAT_VP9:
                *plugin = MFX_PLUGINID_VP9D_HW;
                ret = 0;
                break;
            default:
                break;
            }
        }
    }

    return ret;
}

void virtio_video_msdk_load_plugin(VirtIODevice *vdev, virtio_video_format format, bool encode, bool unload)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    mfxStatus sts = MFX_ERR_NONE;
    mfxPluginUID pluginUID = {0};

    if (virtio_video_msdk_get_plugin(format, encode, &pluginUID) == 0) {
        if (unload) {
            sts = MFXVideoUSER_UnLoad((mfxSession)vid->mfx_session, &pluginUID);
            VIRTVID_VERBOSE("Unload MFX plugin for format %x, status %d", format, sts);
        } else {
            sts = MFXVideoUSER_Load((mfxSession)vid->mfx_session, &pluginUID, vid->mfx_version_major);
            VIRTVID_VERBOSE("Load MFX plugin for format %x, status %d", format, sts);
        }
    }
}

int virito_video_format_to_mfx4cc(virtio_video_format fmt)
{
    int mfx4cc = 0;

    switch (fmt) {
    // Raw format
    case VIRTIO_VIDEO_FORMAT_ARGB8888:
        mfx4cc = MFX_FOURCC_RGB4;
        break;
    case VIRTIO_VIDEO_FORMAT_BGRA8888:
        /* Unsupported */
        break;
    case VIRTIO_VIDEO_FORMAT_NV12:
        mfx4cc = MFX_FOURCC_NV12;
        break;
    case VIRTIO_VIDEO_FORMAT_YUV420:
        mfx4cc = MFX_FOURCC_IYUV;
        break;
    case VIRTIO_VIDEO_FORMAT_YVU420:
        mfx4cc = MFX_FOURCC_YV12;
        break;
    // Coded format
    case VIRTIO_VIDEO_FORMAT_MPEG2:
        mfx4cc = MFX_CODEC_MPEG2;
        break;
    case VIRTIO_VIDEO_FORMAT_MPEG4:
        /* Unsupported */
        break;
    case VIRTIO_VIDEO_FORMAT_H264:
        mfx4cc = MFX_CODEC_AVC;
        break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
        mfx4cc = MFX_CODEC_HEVC;
        break;
    case VIRTIO_VIDEO_FORMAT_VP8:
        mfx4cc = MFX_CODEC_VP8;
        break;
    case VIRTIO_VIDEO_FORMAT_VP9:
        mfx4cc = MFX_CODEC_VP9;
        break;
    default:
        break;
    }

    return mfx4cc;
}

virtio_video_format virito_video_format_from_mfx4cc(int mfx4cc)
{
    virtio_video_format fmt = VIRTIO_VIDEO_FORMAT_RAW_MAX;

    switch (mfx4cc) {
    // Raw format
    case MFX_FOURCC_RGB4:
        fmt = VIRTIO_VIDEO_FORMAT_ARGB8888;
        break;
    case MFX_FOURCC_NV12:
        fmt = VIRTIO_VIDEO_FORMAT_NV12;
        break;
    case MFX_FOURCC_IYUV:
        fmt = VIRTIO_VIDEO_FORMAT_YUV420;
        break;
    case MFX_FOURCC_YV12:
        fmt = VIRTIO_VIDEO_FORMAT_YVU420;
        break;
    // Coded format
    case MFX_CODEC_MPEG2:
        fmt = VIRTIO_VIDEO_FORMAT_MPEG2;
        break;
    case MFX_CODEC_AVC:
        fmt = VIRTIO_VIDEO_FORMAT_H264;
        break;
    case MFX_CODEC_HEVC:
        fmt = VIRTIO_VIDEO_FORMAT_HEVC;
        break;
    case MFX_CODEC_VP8:
        fmt = VIRTIO_VIDEO_FORMAT_VP8;
        break;
    case MFX_CODEC_VP9:
        fmt = VIRTIO_VIDEO_FORMAT_VP9;
        break;
    default:
        break;
    }

    return fmt;
}
