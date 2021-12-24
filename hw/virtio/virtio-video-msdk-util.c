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

uint32_t virtio_video_msdk_to_profile(uint32_t msdk)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(profile_table); i++) {
        if (profile_table[i].msdk_value == msdk)
            return profile_table[i].virtio_value;
    }
    return 0;
}

uint32_t virtio_video_msdk_to_level(uint32_t msdk)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(level_table); i++) {
        if (level_table[i].msdk_value == msdk)
            return level_table[i].virtio_value;
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

int virtio_video_msdk_init_param_dec(mfxVideoParam *param,
    VirtIOVideoStream *stream)
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

int virtio_video_msdk_init_vpp_param_dec(mfxVideoParam *param,
    mfxVideoParam *vpp_param, VirtIOVideoStream *stream)
{
    uint32_t msdk_format =
        virtio_video_format_to_msdk(stream->out.params.format);

    if (param == NULL || vpp_param == NULL || msdk_format == 0)
        return -1;

    memset(vpp_param, 0, sizeof(*vpp_param));
    vpp_param->vpp.In.FourCC = MFX_FOURCC_NV12;
    vpp_param->vpp.In.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    vpp_param->vpp.In.CropX = param->mfx.FrameInfo.CropX;
    vpp_param->vpp.In.CropY = param->mfx.FrameInfo.CropY;
    vpp_param->vpp.In.CropW = param->mfx.FrameInfo.CropW;
    vpp_param->vpp.In.CropH = param->mfx.FrameInfo.CropH;
    vpp_param->vpp.In.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    vpp_param->vpp.In.FrameRateExtN = param->mfx.FrameInfo.FrameRateExtN;
    vpp_param->vpp.In.FrameRateExtD = param->mfx.FrameInfo.FrameRateExtD;
    vpp_param->vpp.In.Width = MSDK_ALIGN16(vpp_param->vpp.In.CropW);
    vpp_param->vpp.In.Height = MSDK_ALIGN16(vpp_param->vpp.In.CropH);

    vpp_param->vpp.Out = vpp_param->vpp.In;
    vpp_param->vpp.Out.FourCC = msdk_format;
    switch (msdk_format) {
    case MFX_FOURCC_RGB4:
        vpp_param->vpp.Out.ChromaFormat = MFX_CHROMAFORMAT_YUV444;
        break;
    case MFX_FOURCC_NV12:
    case MFX_FOURCC_IYUV:
    case MFX_FOURCC_YV12:
        vpp_param->vpp.Out.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        break;
    default:
        vpp_param->vpp.Out.ChromaFormat = MFX_CHROMAFORMAT_MONOCHROME;
        break;
    }

    switch(stream->out.mem_type) {
    case VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES:
        vpp_param->IOPattern |= MFX_IOPATTERN_IN_SYSTEM_MEMORY |
                                MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        break;
    case VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT:
        vpp_param->IOPattern |= MFX_IOPATTERN_IN_VIDEO_MEMORY |
                                MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        break;
    default:
        break;
    }

    return 0;
}

void virtio_video_msdk_init_surface_pool(MsdkSession *session,
    mfxFrameAllocRequest *alloc_req, mfxFrameInfo *info, bool vpp)
{
    MsdkSurface *surface;
    mfxU8 *surface_buf;
    uint32_t width, height, size;
    int i, surface_num;

    width = MSDK_ALIGN32(alloc_req->Info.Width);
    height = MSDK_ALIGN32(alloc_req->Info.Height);
    switch (info->FourCC) {
    case MFX_FOURCC_RGB4:
        size = width * height * 4;
        break;
    case MFX_FOURCC_NV12:
    case MFX_FOURCC_IYUV:
    case MFX_FOURCC_YV12:
        size = width * height * 12 / 8;
        break;
    default:
        return;
    }

    surface_num = vpp ? session->vpp_surface_num : session->surface_num;
    printf("dyang23, virtio_video_msdk_init_surface_pool create:%d surface of surface pool \n", surface_num);
    for (i = 0; i < surface_num; i++) {
        surface = g_new0(MsdkSurface, 1);
        surface_buf = g_malloc0(size);

        surface->used = false;
        surface->surface.Info = *info;
        switch (info->FourCC) {
        case MFX_FOURCC_RGB4:
            surface->surface.Data.B = surface_buf;
            surface->surface.Data.G = surface->surface.Data.B + 1;
            surface->surface.Data.R = surface->surface.Data.B + 2;
            surface->surface.Data.A = surface->surface.Data.B + 3;
            surface->surface.Data.PitchLow = width * 4;
            surface->surface.Data.PitchHigh = 0;
            break;
        case MFX_FOURCC_NV12:
            surface->surface.Data.Y = surface_buf;
            surface->surface.Data.UV = surface->surface.Data.Y + width * height;
            surface->surface.Data.PitchLow = width;
            surface->surface.Data.PitchHigh = 0;
            break;
        case MFX_FOURCC_IYUV:
            surface->surface.Data.Y = surface_buf;
            surface->surface.Data.U = surface->surface.Data.Y + width * height;
            surface->surface.Data.V = surface->surface.Data.U + width * height / 4;
            surface->surface.Data.PitchLow = width;
            surface->surface.Data.PitchHigh = 0;
            break;
        case MFX_FOURCC_YV12:
            surface->surface.Data.Y = surface_buf;
            surface->surface.Data.V = surface->surface.Data.Y + width * height;
            surface->surface.Data.U = surface->surface.Data.V + width * height / 4;
            surface->surface.Data.PitchLow = width;
            surface->surface.Data.PitchHigh = 0;
            break;
        default:
            break;
        }

        if (vpp) {
            QLIST_INSERT_HEAD(&session->vpp_surface_pool, surface, next);
        } else {
            QLIST_INSERT_HEAD(&session->surface_pool, surface, next);
        }
    }
}

static void virtio_video_msdk_uninit_surface(MsdkSurface *surface)
{
    switch (surface->surface.Info.FourCC) {
    case MFX_FOURCC_RGB4:
        g_free(surface->surface.Data.B);
        break;
    case MFX_FOURCC_NV12:
    case MFX_FOURCC_IYUV:
    case MFX_FOURCC_YV12:
        g_free(surface->surface.Data.Y);
        break;
    default:
        break;
    }
    g_free(surface);
}

void virtio_video_msdk_uninit_surface_pools(MsdkSession *session)
{
    MsdkSurface *surface, *tmp_surface;

    QLIST_FOREACH_SAFE(surface, &session->surface_pool, next, tmp_surface) {
        QLIST_REMOVE(surface, next);
        virtio_video_msdk_uninit_surface(surface);
    }
    QLIST_FOREACH_SAFE(surface, &session->vpp_surface_pool, next, tmp_surface) {
        QLIST_REMOVE(surface, next);
        virtio_video_msdk_uninit_surface(surface);
    }
}

int virtio_video_msdk_output_surface(MsdkSurface *surface,
    VirtIOVideoResource *resource)
{
    mfxFrameSurface1 *frame = &surface->surface;
    uint32_t width, height;
    int ret = 0;

    width = frame->Info.Width;
    height = frame->Info.Height;
    switch (frame->Info.FourCC) {
    case MFX_FOURCC_RGB4:
        if (resource->num_planes != 1)
            goto error;

        ret += virtio_video_memcpy(resource, 0, frame->Data.B, width * height * 4);
        break;
    case MFX_FOURCC_NV12:
        if (resource->num_planes != 2)
            goto error;

        ret += virtio_video_memcpy(resource, 0, frame->Data.Y, width * height);
        ret += virtio_video_memcpy(resource, 1, frame->Data.U, width * height / 2);
        break;
    case MFX_FOURCC_IYUV:
        if (resource->num_planes != 3)
            goto error;

        ret += virtio_video_memcpy(resource, 0, frame->Data.Y, width * height);
        ret += virtio_video_memcpy(resource, 0, frame->Data.U, width * height / 4);
        ret += virtio_video_memcpy(resource, 0, frame->Data.V, width * height / 4);
        break;
    case MFX_FOURCC_YV12:
        if (resource->num_planes != 3)
            goto error;

        ret += virtio_video_memcpy(resource, 0, frame->Data.Y, width * height);
        ret += virtio_video_memcpy(resource, 0, frame->Data.V, width * height / 4);
        ret += virtio_video_memcpy(resource, 0, frame->Data.U, width * height / 4);
        break;
    default:
        break;
    }

    surface->used = false;
    return ret < 0 ? -1 : 0;

error:
    surface->used = false;
    return -1;
}

static const mfxPluginUID* virtio_video_msdk_find_plugin(uint32_t format,
    bool encode)
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
void virtio_video_msdk_load_plugin(mfxSession session, uint32_t format,
    bool encode)
{
    const mfxPluginUID *pluginUID;

    if (session == NULL) {
        return;
    }

    pluginUID = virtio_video_msdk_find_plugin(format, encode);
    if (pluginUID != NULL) {
        MFXVideoUSER_Load(session, pluginUID, 1);
    }
}

void virtio_video_msdk_unload_plugin(mfxSession session, uint32_t format,
    bool encode)
{
    const mfxPluginUID *pluginUID;

    if (session == NULL) {
        return;
    }

    pluginUID = virtio_video_msdk_find_plugin(format, encode);
    if (pluginUID != NULL) {
        MFXVideoUSER_UnLoad(session, pluginUID);
    }
}

int virtio_video_msdk_init_handle(VirtIOVideo *v)
{
    MsdkHandle *msdk = g_new(MsdkHandle, 1);
    VAStatus va_status;
    int major, minor;

    msdk->drm_fd = open(VIRTIO_VIDEO_DRM_DEVICE, O_RDWR);
    if (msdk->drm_fd < 0) {
        error_report("Failed to open %s", VIRTIO_VIDEO_DRM_DEVICE);
        g_free(msdk);
        return -1;
    }

    msdk->va_handle = vaGetDisplayDRM(msdk->drm_fd);
    if (!msdk->va_handle) {
        error_report("Failed to get VA display for %s",
                     VIRTIO_VIDEO_DRM_DEVICE);
        close(msdk->drm_fd);
        g_free(msdk);
        return -1;
    }

    va_status = vaInitialize(msdk->va_handle, &major, &minor);
    if (va_status != VA_STATUS_SUCCESS) {
        error_report("vaInitialize failed: %d", va_status);
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
    MsdkHandle *msdk = (MsdkHandle *) v->opaque;

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

void printf_mfxVideoParam(mfxVideoParam *mfxVideoParam) {
    printf("mfxVideoParam->mfx.CodecId = 0x%x\n", mfxVideoParam->mfx.CodecId);
    printf("mfxVideoParam->mfx.CodecProfile = %d\n", mfxVideoParam->mfx.CodecProfile);
    printf("mfxVideoParam->mfx.CodecLevel = %d\n", mfxVideoParam->mfx.CodecLevel);
    printf("mfxVideoParam->mfx.NumThread = %d\n", mfxVideoParam->mfx.NumThread);
    printf("mfxVideoParam->mfx.FrameInfo.FourCC = 0x%x\n", mfxVideoParam->mfx.FrameInfo.FourCC);
    printf("mfxVideoParam->mfx.FrameInfo.Width = %d\n", mfxVideoParam->mfx.FrameInfo.Width);
    printf("mfxVideoParam->mfx.FrameInfo.Height = %d\n", mfxVideoParam->mfx.FrameInfo.Height);
    printf("mfxVideoParam->mfx.FrameInfo.CropX = %d\n", mfxVideoParam->mfx.FrameInfo.CropX);
    printf("mfxVideoParam->mfx.FrameInfo.CropY = %d\n", mfxVideoParam->mfx.FrameInfo.CropY);
    printf("mfxVideoParam->mfx.FrameInfo.CropW = %d\n", mfxVideoParam->mfx.FrameInfo.CropW);
    printf("mfxVideoParam->mfx.FrameInfo.CropH = %d\n", mfxVideoParam->mfx.FrameInfo.CropH);
    printf("mfxVideoParam->mfx.FrameInfo.FrameRateExtN = %d\n", mfxVideoParam->mfx.FrameInfo.FrameRateExtN);
    printf("mfxVideoParam->mfx.FrameInfo.FrameRateExtD = %d\n", mfxVideoParam->mfx.FrameInfo.FrameRateExtD);
}

char * virtio_video_fmt_to_string(virtio_video_format fmt){
    switch (fmt) {
        case VIRTIO_VIDEO_FORMAT_ARGB8888:
            return (char *)"VIRTIO_VIDEO_FORMAT_ARGB8888";
        case VIRTIO_VIDEO_FORMAT_BGRA8888:
            return (char *)"VIRTIO_VIDEO_FORMAT_BGRA8888";
        case VIRTIO_VIDEO_FORMAT_NV12:
            return (char *)"VIRTIO_VIDEO_FORMAT_NV12";
        case VIRTIO_VIDEO_FORMAT_YUV420:
            return (char *)"VIRTIO_VIDEO_FORMAT_YUV420";
        case VIRTIO_VIDEO_FORMAT_YVU420:
            return (char *)"VIRTIO_VIDEO_FORMAT_YVU420";
        case VIRTIO_VIDEO_FORMAT_MPEG2:
            return (char *)"VIRTIO_VIDEO_FORMAT_MPEG2";
        case VIRTIO_VIDEO_FORMAT_MPEG4:
            return (char *)"VIRTIO_VIDEO_FORMAT_MPEG4";
        case VIRTIO_VIDEO_FORMAT_H264:
            return (char *)"VIRTIO_VIDEO_FORMAT_H264";
        case VIRTIO_VIDEO_FORMAT_HEVC:
            return (char *)"VIRTIO_VIDEO_FORMAT_HEVC";
        case VIRTIO_VIDEO_FORMAT_VP8:
            return (char *)"VIRTIO_VIDEO_FORMAT_VP8";
        case VIRTIO_VIDEO_FORMAT_VP9:
            return (char *)"VIRTIO_VIDEO_FORMAT_VP9";
        default:
            return (char *)"unknown format";
    }
}