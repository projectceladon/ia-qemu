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
#include "virtio-video-msdk.h"
#include "virtio-video-msdk-util.h"
#include "mfx/mfxplugin.h"
#include "mfx/mfxvp8.h"
#include "va/va_drm.h"

#define VIRTIO_VIDEO_DRM_DEVICE "/dev/dri/by-path/pci-0000:00:02.0-render"

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

int virtio_video_msdk_init_param(mfxVideoParam *param, uint32_t format)
{
    uint32_t msdk_format = virtio_video_format_to_msdk(format);

    if (param == NULL || msdk_format == 0)
        return -1;

    /* Hardware usually only supports NV12 pixel format */
    memset(param, 0, sizeof(*param));
    param->mfx.CodecId = msdk_format;
    param->mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    param->mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    param->mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    return 0;
}

int virtio_video_msdk_init_param_dec(mfxVideoParam *param, VirtIOVideoStream *stream)
{

    if (virtio_video_msdk_init_param(param, stream->in.params.format) < 0)
        return -1;

    switch (stream->in.mem_type) {
    case VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES:
        param->IOPattern |= MFX_IOPATTERN_IN_SYSTEM_MEMORY;
        break;
    case VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT:
        param->IOPattern |= MFX_IOPATTERN_IN_VIDEO_MEMORY;
        break;
    default:
        break;
    }
    switch (stream->out.mem_type) {
    case VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES:
        param->IOPattern |= MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        break;
    case VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT:
        param->IOPattern |= MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        break;
    default:
        break;
    }

    return 0;
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

int virtio_video_msdk_init_handle(VirtIOVideo *v)
{
    VirtIOVideoMediaSDK *msdk = g_malloc(sizeof(VirtIOVideoMediaSDK));
    VAStatus va_status;
    int major, minor;

    msdk->drm_fd = open(VIRTIO_VIDEO_DRM_DEVICE, O_RDWR);
    if (msdk->drm_fd < 0) {
        VIRTVID_ERROR("error open DRM_DEVICE %s\n", VIRTIO_VIDEO_DRM_DEVICE);
        g_free(msdk);
        return -1;
    }

    msdk->va_handle = vaGetDisplayDRM(msdk->drm_fd);
    if (!msdk->va_handle) {
        VIRTVID_ERROR("error vaGetDisplayDRM for %s\n", VIRTIO_VIDEO_DRM_DEVICE);
        close(msdk->drm_fd);
        g_free(msdk);
        return -1;
    }

    va_status = vaInitialize(msdk->va_handle, &major, &minor);
    if (va_status != VA_STATUS_SUCCESS) {
        VIRTVID_ERROR("error vaInitialize for %s, status %d\n",
                VIRTIO_VIDEO_DRM_DEVICE, va_status);
        vaTerminate(msdk->va_handle);
        close(msdk->drm_fd);
        g_free(msdk);
        return -1;
    }

    v->opaque = msdk;
    return 0;
}

void virtio_video_msdk_uninit_handle(VirtIOVideo *v)
{
    VirtIOVideoMediaSDK *msdk = (VirtIOVideoMediaSDK *) v->opaque;

    if (msdk->va_handle) {
        vaTerminate(msdk->va_handle);
        msdk->va_handle = NULL;
    }

    if (msdk->drm_fd) {
        close(msdk->drm_fd);
        msdk->drm_fd = 0;
    }

    g_free(msdk);
    v->opaque = NULL;
}
