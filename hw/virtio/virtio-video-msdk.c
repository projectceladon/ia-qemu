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

struct virtio_video_convert_table {
    uint32_t virtio_value;
    uint32_t msdk_value;
};

static struct virtio_video_convert_table format_table[] = {
    /* Raw Formats */
    { VIRTIO_VIDEO_FORMAT_ARGB8888, MFX_FOURCC_RGB4 },
    { VIRTIO_VIDEO_FORMAT_BGRA8888, 0 },
    { VIRTIO_VIDEO_FORMAT_NV12, MFX_FOURCC_NV12 },
    { VIRTIO_VIDEO_FORMAT_YUV420, MFX_FOURCC_IYUV },
    { VIRTIO_VIDEO_FORMAT_YVU420, MFX_FOURCC_YV12 },
    /* Coded Formats */
    { VIRTIO_VIDEO_FORMAT_MPEG2, MFX_CODEC_MPEG2 },
    { VIRTIO_VIDEO_FORMAT_MPEG4, 0 },
    { VIRTIO_VIDEO_FORMAT_H264, MFX_CODEC_AVC },
    { VIRTIO_VIDEO_FORMAT_HEVC, MFX_CODEC_HEVC },
    { VIRTIO_VIDEO_FORMAT_VP8, MFX_CODEC_VP8 },
    { VIRTIO_VIDEO_FORMAT_VP9, MFX_CODEC_VP9 },
    { 0 },
};

static struct virtio_video_convert_table profile_table[] = {
    /* H264 */
    {VIRTIO_VIDEO_PROFILE_H264_BASELINE, MFX_PROFILE_AVC_BASELINE},
    {VIRTIO_VIDEO_PROFILE_H264_MAIN, MFX_PROFILE_AVC_MAIN},
    {VIRTIO_VIDEO_PROFILE_H264_EXTENDED, MFX_PROFILE_AVC_EXTENDED},
    {VIRTIO_VIDEO_PROFILE_H264_HIGH, MFX_PROFILE_AVC_HIGH},
    {VIRTIO_VIDEO_PROFILE_H264_HIGH10PROFILE, MFX_PROFILE_AVC_HIGH10},
    {VIRTIO_VIDEO_PROFILE_H264_HIGH422PROFILE, MFX_PROFILE_AVC_HIGH_422},
    {VIRTIO_VIDEO_PROFILE_H264_HIGH444PREDICTIVEPROFILE, 0},
    {VIRTIO_VIDEO_PROFILE_H264_SCALABLEBASELINE, 0},
    {VIRTIO_VIDEO_PROFILE_H264_SCALABLEHIGH, 0},
    {VIRTIO_VIDEO_PROFILE_H264_STEREOHIGH, 0},
    {VIRTIO_VIDEO_PROFILE_H264_MULTIVIEWHIGH, 0},
    /* HEVC */
    {VIRTIO_VIDEO_PROFILE_HEVC_MAIN, MFX_PROFILE_HEVC_MAIN},
    {VIRTIO_VIDEO_PROFILE_HEVC_MAIN10, MFX_PROFILE_HEVC_MAIN10},
    {VIRTIO_VIDEO_PROFILE_HEVC_MAIN_STILL_PICTURE, MFX_PROFILE_HEVC_MAINSP},
    /* VP8 */
    {VIRTIO_VIDEO_PROFILE_VP8_PROFILE0, MFX_PROFILE_VP8_0},
    {VIRTIO_VIDEO_PROFILE_VP8_PROFILE1, MFX_PROFILE_VP8_1},
    {VIRTIO_VIDEO_PROFILE_VP8_PROFILE2, MFX_PROFILE_VP8_2},
    {VIRTIO_VIDEO_PROFILE_VP8_PROFILE3, MFX_PROFILE_VP8_3},
    /* VP9 */
    {VIRTIO_VIDEO_PROFILE_VP9_PROFILE0, MFX_PROFILE_VP9_0},
    {VIRTIO_VIDEO_PROFILE_VP9_PROFILE1, MFX_PROFILE_VP9_1},
    {VIRTIO_VIDEO_PROFILE_VP9_PROFILE2, MFX_PROFILE_VP9_2},
    {VIRTIO_VIDEO_PROFILE_VP9_PROFILE3, MFX_PROFILE_VP9_3},
};

static struct virtio_video_convert_table level_table[] = {
    /* H264 */
    {VIRTIO_VIDEO_LEVEL_H264_1_0, MFX_LEVEL_AVC_1},
    {VIRTIO_VIDEO_LEVEL_H264_1_1, MFX_LEVEL_AVC_11},
    {VIRTIO_VIDEO_LEVEL_H264_1_2, MFX_LEVEL_AVC_12},
    {VIRTIO_VIDEO_LEVEL_H264_1_3, MFX_LEVEL_AVC_13},
    {VIRTIO_VIDEO_LEVEL_H264_2_0, MFX_LEVEL_AVC_2},
    {VIRTIO_VIDEO_LEVEL_H264_2_1, MFX_LEVEL_AVC_21},
    {VIRTIO_VIDEO_LEVEL_H264_2_2, MFX_LEVEL_AVC_22},
    {VIRTIO_VIDEO_LEVEL_H264_3_0, MFX_LEVEL_AVC_3},
    {VIRTIO_VIDEO_LEVEL_H264_3_1, MFX_LEVEL_AVC_31},
    {VIRTIO_VIDEO_LEVEL_H264_3_2, MFX_LEVEL_AVC_32},
    {VIRTIO_VIDEO_LEVEL_H264_4_0, MFX_LEVEL_AVC_4},
    {VIRTIO_VIDEO_LEVEL_H264_4_1, MFX_LEVEL_AVC_41},
    {VIRTIO_VIDEO_LEVEL_H264_4_2, MFX_LEVEL_AVC_42},
    {VIRTIO_VIDEO_LEVEL_H264_5_0, MFX_LEVEL_AVC_5},
    {VIRTIO_VIDEO_LEVEL_H264_5_1, MFX_LEVEL_AVC_51},
    /* HEVC */
    {VIRTIO_VIDEO_LEVEL_HEVC_1_0, MFX_LEVEL_HEVC_1},
    {VIRTIO_VIDEO_LEVEL_HEVC_2_0, MFX_LEVEL_HEVC_2},
    {VIRTIO_VIDEO_LEVEL_HEVC_2_1, MFX_LEVEL_HEVC_21},
    {VIRTIO_VIDEO_LEVEL_HEVC_3_0, MFX_LEVEL_HEVC_3},
    {VIRTIO_VIDEO_LEVEL_HEVC_3_1, MFX_LEVEL_HEVC_31},
    {VIRTIO_VIDEO_LEVEL_HEVC_4_0, MFX_LEVEL_HEVC_4},
    {VIRTIO_VIDEO_LEVEL_HEVC_4_1, MFX_LEVEL_HEVC_41},
    {VIRTIO_VIDEO_LEVEL_HEVC_5_0, MFX_LEVEL_HEVC_5},
    {VIRTIO_VIDEO_LEVEL_HEVC_5_1, MFX_LEVEL_HEVC_51},
    {VIRTIO_VIDEO_LEVEL_HEVC_5_2, MFX_LEVEL_HEVC_52},
    {VIRTIO_VIDEO_LEVEL_HEVC_6_0, MFX_LEVEL_HEVC_6},
    {VIRTIO_VIDEO_LEVEL_HEVC_6_1, MFX_LEVEL_HEVC_61},
    {VIRTIO_VIDEO_LEVEL_HEVC_6_2, MFX_LEVEL_HEVC_62},
};

uint32_t virtio_video_format_to_msdk(uint32_t format)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(format_table); i++) {
        if (format_table[i].virtio_value == format)
            return format_table[i].msdk_value;
    }

    return 0;
}

uint32_t virtio_video_profile_to_msdk(uint32_t profile)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(profile_table); i++) {
        if (profile_table[i].virtio_value == profile)
            return profile_table[i].msdk_value;
    }
    return 0;
}

uint32_t virtio_video_level_to_msdk(uint32_t level)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(level_table); i++) {
        if (level_table[i].virtio_value == level)
            return level_table[i].msdk_value;
    }
    return 0;
}

int virtio_video_profile_range(uint32_t format, uint32_t *min, uint32_t *max)
{
    if (min == NULL || max == NULL) {
        return -1;
    }

    switch (format) {
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
        return -1;
    }

    return 0;
}

int virtio_video_level_range(uint32_t format, uint32_t *min, uint32_t *max)
{
    if (min == NULL || max == NULL) {
        return -1;
    }

    switch (format) {
    case VIRTIO_VIDEO_FORMAT_H264:
        *min = VIRTIO_VIDEO_LEVEL_H264_MIN;
        *max = VIRTIO_VIDEO_LEVEL_H264_MAX;
        break;
    case VIRTIO_VIDEO_FORMAT_HEVC:
        *min = VIRTIO_VIDEO_LEVEL_HEVC_MIN;
        *max = VIRTIO_VIDEO_LEVEL_HEVC_MAX;
        break;
    default:
        return -1;
    }

    return 0;
}

void virtio_video_msdk_init_video_params(mfxVideoParam *param, uint32_t format)
{
    uint32_t msdk_format = virtio_video_format_to_msdk(format);

    if (param == NULL || msdk_format == 0) {
        return;
    }

    /* Hardware usually only supports NV12 pixel format */
    memset(param, 0, sizeof(*param));
    param->mfx.CodecId = msdk_format;
    param->mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    param->mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    param->mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
}

void virtio_video_msdk_init_format(VirtIOVideoFormat *fmt, uint32_t format)
{
    if (fmt == NULL) {
        return;
    }

    QLIST_INIT(&fmt->frames);
    fmt->desc.mask = 0;
    fmt->desc.format = format;
    fmt->desc.planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER;
    fmt->desc.plane_align = 0;
    fmt->desc.num_frames = 0;

    fmt->profile.num = 0;
    fmt->profile.values = NULL;
    fmt->level.num = 0;
    fmt->level.values = NULL;
}

static const mfxPluginUID* virtio_video_msdk_find_plugin(uint32_t format, bool encode)
{
    if (encode) {
        switch (format) {
        case VIRTIO_VIDEO_FORMAT_HEVC:
            return &MFX_PLUGINID_HEVCE_HW;
        case VIRTIO_VIDEO_FORMAT_VP8:
            return &MFX_PLUGINID_VP8E_HW;
        case VIRTIO_VIDEO_FORMAT_VP9:
            return &MFX_PLUGINID_VP9E_HW;
        default:
            break;
        }
    } else {
        switch (format) {
        case VIRTIO_VIDEO_FORMAT_HEVC:
            return &MFX_PLUGINID_HEVCD_HW;
        case VIRTIO_VIDEO_FORMAT_VP8:
            return &MFX_PLUGINID_VP8D_HW;
        case VIRTIO_VIDEO_FORMAT_VP9:
            return &MFX_PLUGINID_VP9D_HW;
        default:
            break;
        }
    }

    return NULL;
}

/* Load plugin if required */
void virtio_video_msdk_load_plugin(mfxSession session, uint32_t format, bool encode)
{
    mfxStatus status;
    const mfxPluginUID *pluginUID;

    if (session == NULL) {
        return;
    }

    pluginUID = virtio_video_msdk_find_plugin(format, encode);
    if (pluginUID != NULL) {
        status = MFXVideoUSER_Load(session, pluginUID, 1);
        VIRTVID_VERBOSE("Load MFX plugin for format %x, status %d", format, status);
    }
}

void virtio_video_msdk_unload_plugin(mfxSession session, uint32_t format, bool encode)
{
    mfxStatus status;
    const mfxPluginUID *pluginUID;

    if (session == NULL) {
        return;
    }

    pluginUID = virtio_video_msdk_find_plugin(format, encode);
    if (pluginUID != NULL) {
        status = MFXVideoUSER_UnLoad(session, pluginUID);
        VIRTVID_VERBOSE("Unload MFX plugin for format %x, status %d", format, status);
    }
}
