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
        /* Only support NV12 for now */
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

void virtio_video_msdk_init_format(virtio_video_format format, virtio_video_format_desc *format_desc)
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

void virtio_video_msdk_load_plugin(mfxSession mfx_session, virtio_video_format format, bool encode, bool unload)
{
    mfxStatus sts = MFX_ERR_NONE;
    mfxPluginUID pluginUID = {0};

    if (mfx_session == NULL) {
        return;
    }

    if (virtio_video_msdk_get_plugin(format, encode, &pluginUID) == 0) {
        if (unload) {
            sts = MFXVideoUSER_UnLoad(mfx_session, &pluginUID);
            VIRTVID_VERBOSE("Unload MFX plugin for format %x, status %d", format, sts);
        } else {
            sts = MFXVideoUSER_Load(mfx_session, &pluginUID, 1);
            VIRTVID_VERBOSE("Load MFX plugin for format %x, status %d", format, sts);
        }
    }
}

int virito_video_format_to_mfx4cc(virtio_video_format fmt)
{
    int mfx4cc = 0;

    switch (fmt) {
    /* Raw format */
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
    /* Coded format */
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
    /* Raw format */
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
    /* Coded format */
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

void virtio_video_profile_range(virtio_video_format fmt, int *min, int *max)
{
    if (min == NULL || max == NULL) {
        return;
    }

    *min = 0;
    *max = 0;

    switch (fmt) {
    case VIRTIO_VIDEO_FORMAT_MPEG2:
        *min = VIRTIO_VIDEO_PROFILE_MPEG2_MIN;
        *max = VIRTIO_VIDEO_PROFILE_MPEG2_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_H264:
        *min = VIRTIO_VIDEO_PROFILE_H264_MIN;
        *max = VIRTIO_VIDEO_PROFILE_H264_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
        *min = VIRTIO_VIDEO_PROFILE_HEVC_MIN;
        *max = VIRTIO_VIDEO_PROFILE_HEVC_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_VP8:
        *min = VIRTIO_VIDEO_PROFILE_VP8_MIN;
        *max = VIRTIO_VIDEO_PROFILE_VP8_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_VP9:
        *min = VIRTIO_VIDEO_PROFILE_VP9_MIN;
        *max = VIRTIO_VIDEO_PROFILE_VP9_MAX;
        break;
    default:
        break;
    }
}

void virtio_video_level_range(virtio_video_format fmt, int *min, int *max)
{
    if (min == NULL || max == NULL) {
        return;
    }

    *min = 0;
    *max = 0;

    switch (fmt) {
    case VIRTIO_VIDEO_FORMAT_MPEG2:
        *min = VIRTIO_VIDEO_LEVEL_MPEG2_MIN;
        *max = VIRTIO_VIDEO_LEVEL_MPEG2_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_H264:
        *min = VIRTIO_VIDEO_LEVEL_H264_MIN;
        *max = VIRTIO_VIDEO_LEVEL_H264_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
        *min = VIRTIO_VIDEO_LEVEL_HEVC_MIN;
        *max = VIRTIO_VIDEO_LEVEL_HEVC_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_VP8:
        *min = VIRTIO_VIDEO_LEVEL_VP8_MIN;
        *max = VIRTIO_VIDEO_LEVEL_VP8_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_VP9:
        *min = VIRTIO_VIDEO_LEVEL_VP9_MIN;
        *max = VIRTIO_VIDEO_LEVEL_VP9_MAX;
        break;
    default:
        break;
    }
}

int virtio_video_profile_to_mfx(virtio_video_format fmt, virtio_video_profile profile)
{
    int mfx_profile = MFX_PROFILE_UNKNOWN;

    switch (fmt) {
    case VIRTIO_VIDEO_FORMAT_MPEG2:
    {
        switch (profile) {
        case VIRTIO_VIDEO_PROFILE_MPEG2_SIMPLE:
            mfx_profile = MFX_PROFILE_MPEG2_SIMPLE;
            break;
        case VIRTIO_VIDEO_PROFILE_MPEG2_MAIN:
            mfx_profile = MFX_PROFILE_MPEG2_MAIN;
            break;
        case VIRTIO_VIDEO_PROFILE_MPEG2_HIGH:
            mfx_profile = MFX_PROFILE_MPEG2_HIGH;
            break;
        default:
            break;
        }
    }
    break;
    case VIRTIO_VIDEO_FORMAT_H264:
    {
        switch (profile) {
        case VIRTIO_VIDEO_PROFILE_H264_BASELINE:
            mfx_profile = MFX_PROFILE_AVC_BASELINE;
            break;
        case VIRTIO_VIDEO_PROFILE_H264_MAIN:
            mfx_profile = MFX_PROFILE_AVC_MAIN;
            break;
        case VIRTIO_VIDEO_PROFILE_H264_EXTENDED:
            mfx_profile = MFX_PROFILE_AVC_EXTENDED;
            break;
        case VIRTIO_VIDEO_PROFILE_H264_HIGH:
            mfx_profile = MFX_PROFILE_AVC_HIGH;
            break;
        case VIRTIO_VIDEO_PROFILE_H264_HIGH10PROFILE:
            mfx_profile = MFX_PROFILE_AVC_HIGH10;
            break;
        case VIRTIO_VIDEO_PROFILE_H264_HIGH422PROFILE:
            mfx_profile = MFX_PROFILE_AVC_HIGH_422;
            break;
        case VIRTIO_VIDEO_PROFILE_H264_HIGH444PREDICTIVEPROFILE:
        case VIRTIO_VIDEO_PROFILE_H264_SCALABLEBASELINE:
        case VIRTIO_VIDEO_PROFILE_H264_SCALABLEHIGH:
        case VIRTIO_VIDEO_PROFILE_H264_STEREOHIGH:
        case VIRTIO_VIDEO_PROFILE_H264_MULTIVIEWHIGH:
            break;
        default:
            break;
        }
    }
    break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
    {
        switch (profile) {
        case VIRTIO_VIDEO_PROFILE_HEVC_MAIN:
            mfx_profile = MFX_PROFILE_HEVC_MAIN;
            break;
        case VIRTIO_VIDEO_PROFILE_HEVC_MAIN10:
            mfx_profile = MFX_PROFILE_HEVC_MAIN10;
            break;
        case VIRTIO_VIDEO_PROFILE_HEVC_MAIN_STILL_PICTURE:
            mfx_profile = MFX_PROFILE_HEVC_MAINSP;
            break;
        default:
            break;
        }
    }
    break;
    case VIRTIO_VIDEO_FORMAT_VP8:
    {
        switch (profile) {
        case VIRTIO_VIDEO_PROFILE_VP8_PROFILE0:
            mfx_profile = MFX_PROFILE_VP8_0;
            break;
        case VIRTIO_VIDEO_PROFILE_VP8_PROFILE1:
            mfx_profile = MFX_PROFILE_VP8_1;
            break;
        case VIRTIO_VIDEO_PROFILE_VP8_PROFILE2:
            mfx_profile = MFX_PROFILE_VP8_2;
            break;
        case VIRTIO_VIDEO_PROFILE_VP8_PROFILE3:
            mfx_profile = MFX_PROFILE_VP8_3;
            break;
        default:
            break;
        }
    }
    break;
    case VIRTIO_VIDEO_FORMAT_VP9:
    {
        switch (profile) {
        case VIRTIO_VIDEO_PROFILE_VP9_PROFILE0:
            mfx_profile = MFX_PROFILE_VP9_0;
            break;
        case VIRTIO_VIDEO_PROFILE_VP9_PROFILE1:
            mfx_profile = MFX_PROFILE_VP9_1;
            break;
        case VIRTIO_VIDEO_PROFILE_VP9_PROFILE2:
            mfx_profile = MFX_PROFILE_VP9_2;
            break;
        case VIRTIO_VIDEO_PROFILE_VP9_PROFILE3:
            mfx_profile = MFX_PROFILE_VP9_3;
            break;
        default:
            break;
        }
    }
    break;
    default:
        break;
    }

    return mfx_profile;
}

int virtio_video_level_to_mfx(virtio_video_format fmt, virtio_video_level level)
{
    int mfx_level = MFX_LEVEL_UNKNOWN;

    switch (fmt) {
    case VIRTIO_VIDEO_FORMAT_MPEG2:
    {
        switch (level) {
        case VIRTIO_VIDEO_LEVEL_MPEG2_LOW:
            mfx_level = MFX_LEVEL_MPEG2_LOW;
            break;
        case VIRTIO_VIDEO_LEVEL_MPEG2_MAIN:
            mfx_level = MFX_LEVEL_MPEG2_MAIN;
            break;
        case VIRTIO_VIDEO_LEVEL_MPEG2_HIGH:
            mfx_level = MFX_LEVEL_MPEG2_HIGH;
            break;
        case VIRTIO_VIDEO_LEVEL_MPEG2_HIGH_1440:
            mfx_level = MFX_LEVEL_MPEG2_HIGH1440;
            break;
        default:
            break;
        }
    }
    break;
    case VIRTIO_VIDEO_FORMAT_H264:
    {
        switch (level) {
        case VIRTIO_VIDEO_LEVEL_H264_1_0:
            mfx_level = MFX_LEVEL_AVC_1;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_1_1:
            mfx_level = MFX_LEVEL_AVC_11;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_1_2:
            mfx_level = MFX_LEVEL_AVC_12;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_1_3:
            mfx_level = MFX_LEVEL_AVC_13;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_2_0:
            mfx_level = MFX_LEVEL_AVC_2;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_2_1:
            mfx_level = MFX_LEVEL_AVC_21;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_2_2:
            mfx_level = MFX_LEVEL_AVC_22;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_3_0:
            mfx_level = MFX_LEVEL_AVC_3;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_3_1:
            mfx_level = MFX_LEVEL_AVC_31;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_3_2:
            mfx_level = MFX_LEVEL_AVC_32;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_4_0:
            mfx_level = MFX_LEVEL_AVC_4;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_4_1:
            mfx_level = MFX_LEVEL_AVC_41;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_4_2:
            mfx_level = MFX_LEVEL_AVC_42;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_5_0:
            mfx_level = MFX_LEVEL_AVC_5;
            break;
        case VIRTIO_VIDEO_LEVEL_H264_5_1:
            mfx_level = MFX_LEVEL_AVC_51;
            break;
        default:
            break;
        }
    }
    break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
    {
        switch (level) {
        case VIRTIO_VIDEO_LEVEL_HEVC_1_0:
            mfx_level = MFX_LEVEL_HEVC_1;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_2_0:
            mfx_level = MFX_LEVEL_HEVC_2;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_2_1:
            mfx_level = MFX_LEVEL_HEVC_21;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_3_0:
            mfx_level = MFX_LEVEL_HEVC_3;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_3_1:
            mfx_level = MFX_LEVEL_HEVC_31;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_4_0:
            mfx_level = MFX_LEVEL_HEVC_4;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_4_1:
            mfx_level = MFX_LEVEL_HEVC_41;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_5_0:
            mfx_level = MFX_LEVEL_HEVC_5;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_5_1:
            mfx_level = MFX_LEVEL_HEVC_51;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_5_2:
            mfx_level = MFX_LEVEL_HEVC_52;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_6_0:
            mfx_level = MFX_LEVEL_HEVC_6;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_6_1:
            mfx_level = MFX_LEVEL_HEVC_61;
            break;
        case VIRTIO_VIDEO_LEVEL_HEVC_6_2:
            mfx_level = MFX_LEVEL_HEVC_62;
            break;
        default:
            break;
        }
    }
    break;
    case VIRTIO_VIDEO_FORMAT_VP8:
        break;
    case VIRTIO_VIDEO_FORMAT_VP9:
        break;
    default:
        break;
    }

    return mfx_level;
}
