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
    vpp_param->vpp.In.FrameRateExtN = param->mfx.FrameInfo.FrameRateExtN ?
                                      param->mfx.FrameInfo.FrameRateExtN : 30;
    vpp_param->vpp.In.FrameRateExtD = param->mfx.FrameInfo.FrameRateExtD ?
                                      param->mfx.FrameInfo.FrameRateExtD : 1;
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
    //if (surface_num < 20)
     //   surface_num = 20;

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

void virtio_video_msdk_uninit_frame(VirtIOVideoFrame *frame)
{
    MsdkFrame *m_frame = frame->opaque;

    if (m_frame != NULL) {
        m_frame->surface->used = false;
        if (m_frame->vpp_surface)
            m_frame->vpp_surface->used = false;
        g_free(m_frame);
    }
    g_free(frame);
}

#define DUMP_SURFACE
#define DUMP_SURFACE_COUNT 200
void virtio_video_msdk_dump_surface(char * src, int len) {
    static FILE *file;
    static int surface_idx=0;
#ifdef DUMP_SURFACE    
    if(surface_idx++ < DUMP_SURFACE_COUNT) {
        if (!file) {
            file = fopen("/tmp/dec.yuv", "w");
            if(!file) {
                printf("openfile failed.\n");
                return;
            }
        }
        if(file)
            fwrite(src, len, 1, file);
    } else {
        if(file) {
            fclose(file);
            file = NULL;
        }
    }
#endif        
        
}

#define DUMP_SURFACE_END
int virtio_video_msdk_output_surface(MsdkSurface *surface,
    VirtIOVideoResource *resource)
{
    mfxFrameSurface1 *frame = &surface->surface;
    uint32_t width, height;
    int ret = 0;
    #ifdef DUMP_SURFACE_END
    char *Y;
    char *U;
    static int i=0;
    #endif

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
        if (resource->planes_layout == VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER)
            ret += virtio_video_memcpy(resource, 0, frame->Data.Y, width * height * 3 / 2);
        else {
            ret += virtio_video_memcpy(resource, 0, frame->Data.Y, width * height);
            ret += virtio_video_memcpy(resource, 1, frame->Data.U, width * height / 2);
        }
        //virtio_video_msdk_dump_surface((char*)(frame->Data.Y), width * height);
        //virtio_video_msdk_dump_surface((char *)(frame->Data.U), width * height/2);
        #ifdef DUMP_SURFACE_END
        if (i++<100) {
            Y = malloc(width*height);
            U = malloc(width*height/2);
            virtio_video_memdump(resource, 0, Y, width * height);
            virtio_video_memdump(resource, 1, U, width * height / 2);
            virtio_video_msdk_dump_surface(Y, width * height);
            virtio_video_msdk_dump_surface(U, width * height/2);
            free(Y);
            free(U);
        }
        #endif
        break;
    case MFX_FOURCC_IYUV:
        if (resource->num_planes != 3)
            goto error;

        ret += virtio_video_memcpy(resource, 0, frame->Data.Y, width * height);
        ret += virtio_video_memcpy(resource, 1, frame->Data.U, width * height / 4);
        ret += virtio_video_memcpy(resource, 2, frame->Data.V, width * height / 4);
        break;
    case MFX_FOURCC_YV12:
        if (resource->num_planes != 3)
            goto error;

        ret += virtio_video_memcpy(resource, 0, frame->Data.Y, width * height);
        ret += virtio_video_memcpy(resource, 1, frame->Data.V, width * height / 4);
        ret += virtio_video_memcpy(resource, 2, frame->Data.U, width * height / 4);
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

void virtio_video_msdk_stream_reset_param(VirtIOVideoStream *stream,
    mfxVideoParam *param)
{
    /* TODO: maybe we should keep crop values set by guest? */
    stream->out.params.frame_width = param->mfx.FrameInfo.Width;
    stream->out.params.frame_height = param->mfx.FrameInfo.Height;
    stream->out.params.frame_rate = param->mfx.FrameInfo.FrameRateExtN /
                                    param->mfx.FrameInfo.FrameRateExtD;
    stream->out.params.crop.left = param->mfx.FrameInfo.CropX;
    stream->out.params.crop.top = param->mfx.FrameInfo.CropY;
    stream->out.params.crop.width = param->mfx.FrameInfo.CropW;
    stream->out.params.crop.height = param->mfx.FrameInfo.CropH;

    switch (stream->out.params.format) {
    case VIRTIO_VIDEO_FORMAT_ARGB8888:
    case VIRTIO_VIDEO_FORMAT_BGRA8888:
        stream->out.params.num_planes = 1;
        stream->out.params.plane_formats[0].plane_size =
            stream->out.params.frame_width * stream->out.params.frame_height * 4;
        stream->out.params.plane_formats[0].stride =
            stream->out.params.frame_width * 4;
        break;
    case VIRTIO_VIDEO_FORMAT_NV12:
        stream->out.params.num_planes = 2;
        stream->out.params.plane_formats[0].plane_size =
            stream->out.params.frame_width * stream->out.params.frame_height;
        stream->out.params.plane_formats[0].stride =
            stream->out.params.frame_width;
        stream->out.params.plane_formats[1].plane_size =
            stream->out.params.frame_width * stream->out.params.frame_height / 2;
        stream->out.params.plane_formats[1].stride =
            stream->out.params.frame_width;
			printf("virtio_video_msdk_stream_reset_param, nv12:plane_size:%d, stride:%d, plane_size:%d, stride:%d\n", 
				stream->out.params.plane_formats[0].plane_size, stream->out.params.plane_formats[0].stride,
				stream->out.params.plane_formats[1].plane_size, stream->out.params.plane_formats[1].stride);
        break;
    case VIRTIO_VIDEO_FORMAT_YUV420:
    case VIRTIO_VIDEO_FORMAT_YVU420:
        stream->out.params.num_planes = 3;
        stream->out.params.plane_formats[0].plane_size =
            stream->out.params.frame_width * stream->out.params.frame_height;
        stream->out.params.plane_formats[0].stride =
            stream->out.params.frame_width;
        stream->out.params.plane_formats[1].plane_size =
            stream->out.params.frame_width * stream->out.params.frame_height / 4;
        stream->out.params.plane_formats[1].stride =
            stream->out.params.frame_width / 2;
        stream->out.params.plane_formats[2].plane_size =
            stream->out.params.frame_width * stream->out.params.frame_height / 4;
        stream->out.params.plane_formats[2].stride =
            stream->out.params.frame_width / 2;
        break;
    default:
        break;
    }

    stream->control.profile =
        virtio_video_msdk_to_profile(param->mfx.CodecProfile);
    stream->control.level = virtio_video_msdk_to_level(param->mfx.CodecLevel);
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

    printf("mfxVideoParam->AllocId = %d\n", mfxVideoParam->AllocId);
    printf("mfxVideoParam->AsyncDepth = %d\n", mfxVideoParam->AsyncDepth);

    printf("mfxVideoParam->mfx.FrameInfo.FourCC = 0x%x\n", mfxVideoParam->mfx.FrameInfo.FourCC);
    printf("mfxVideoParam->mfx.FrameInfo.Width = %d\n", mfxVideoParam->mfx.FrameInfo.Width);
    printf("mfxVideoParam->mfx.FrameInfo.Height = %d\n", mfxVideoParam->mfx.FrameInfo.Height);
    printf("mfxVideoParam->mfx.FrameInfo.CropX = %d\n", mfxVideoParam->mfx.FrameInfo.CropX);
    printf("mfxVideoParam->mfx.FrameInfo.CropY = %d\n", mfxVideoParam->mfx.FrameInfo.CropY);
    printf("mfxVideoParam->mfx.FrameInfo.CropW = %d\n", mfxVideoParam->mfx.FrameInfo.CropW);
    printf("mfxVideoParam->mfx.FrameInfo.CropH = %d\n", mfxVideoParam->mfx.FrameInfo.CropH);
    printf("mfxVideoParam->mfx.FrameInfo.FrameRateExtN = %d\n", mfxVideoParam->mfx.FrameInfo.FrameRateExtN);
    printf("mfxVideoParam->mfx.FrameInfo.FrameRateExtD = %d\n", mfxVideoParam->mfx.FrameInfo.FrameRateExtD);
    printf("mfxVideoParam->mfx.FrameInfo.AspectRatioW = %d\n", mfxVideoParam->mfx.FrameInfo.AspectRatioW);
    printf("mfxVideoParam->mfx.FrameInfo.AspectRatioH = %d\n", mfxVideoParam->mfx.FrameInfo.AspectRatioH);
    printf("mfxVideoParam->mfx.FrameInfo.PicStruct = %d\n", mfxVideoParam->mfx.FrameInfo.PicStruct);
    printf("mfxVideoParam->mfx.FrameInfo.ChromaFormat = %d\n", mfxVideoParam->mfx.FrameInfo.ChromaFormat);
    printf("mfxVideoParam->mfx.CodecId = 0x%x\n", mfxVideoParam->mfx.CodecId);
    printf("mfxVideoParam->mfx.CodecProfile = %d\n", mfxVideoParam->mfx.CodecProfile);
    printf("mfxVideoParam->mfx.CodecLevel = %d\n", mfxVideoParam->mfx.CodecLevel);
    printf("mfxVideoParam->mfx.NumThread = %d\n", mfxVideoParam->mfx.NumThread);
    printf("mfxVideoParam->mfx.DecodedOrder = %d\n", mfxVideoParam->mfx.DecodedOrder);
    printf("mfxVideoParam->mfx.ExtendedPicStruct = %d\n", mfxVideoParam->mfx.ExtendedPicStruct);

    printf("mfxVideoParam->Protected = %d\n", mfxVideoParam->Protected);
    printf("mfxVideoParam->IOPattern = %d\n", mfxVideoParam->IOPattern);
    printf("mfxVideoParam->NumExtParam = %d\n", mfxVideoParam->NumExtParam);
}

void printf_mfxFrameInfo(mfxFrameInfo *mfxInfo){
    printf("mfxFrameInfo->BitDepthLuma = %d\n", mfxInfo->BitDepthLuma);
    printf("mfxFrameInfo->BitDepthChroma = %d\n", mfxInfo->BitDepthChroma);
    printf("mfxFrameInfo->Shift = %d\n", mfxInfo->Shift);

    //printf("mfxFrameInfo->FrameId = %d\n", mfxInfo->FrameId);

    printf("mfxFrameInfo->FourCC = %d\n", mfxInfo->FourCC);
    printf("mfxFrameInfo->Width = %d\n", mfxInfo->Width);
    printf("mfxFrameInfo->Height = %d\n", mfxInfo->Height);

    printf("mfxFrameInfo->CropX = %d\n", mfxInfo->CropX);
    printf("mfxFrameInfo->CropY = %d\n", mfxInfo->CropY);
    printf("mfxFrameInfo->CropW = %d\n", mfxInfo->CropW);
    printf("mfxFrameInfo->CropH = %d\n", mfxInfo->CropH);

    printf("mfxFrameInfo->FrameRateExtN = %d\n", mfxInfo->FrameRateExtN);
    printf("mfxFrameInfo->FrameRateExtD = %d\n", mfxInfo->FrameRateExtD);

    printf("mfxFrameInfo->AspectRatioW = %d\n", mfxInfo->AspectRatioW);
    printf("mfxFrameInfo->AspectRatioH = %d\n", mfxInfo->AspectRatioH);

    printf("mfxFrameInfo->PicStruct = %d\n", mfxInfo->PicStruct);
    printf("mfxFrameInfo->ChromaFormat = %d\n", mfxInfo->ChromaFormat);
}

void printf_mfxFrameData(mfxFrameData *mfxData){
    printf("mfxFrameData->NumExtParam = %d\n", mfxData->NumExtParam);
    printf("mfxFrameData->MemType = %d\n", mfxData->MemType);
    printf("mfxFrameData->PitchHigh = %d\n", mfxData->PitchHigh);

    printf("mfxFrameData->TimeStamp = %ld\n", (long)mfxData->TimeStamp);
    printf("mfxFrameData->FrameOrder = %d\n", mfxData->FrameOrder);
    printf("mfxFrameData->Locked = %d\n", mfxData->Locked);
    printf("mfxFrameData->Pitch = %d\n", mfxData->Pitch);
    printf("mfxFrameData->Y = %p\n", mfxData->Y);
    printf("mfxFrameData->UV = %p\n", mfxData->UV);
    printf("mfxFrameData->V = %p\n", mfxData->V);
    printf("mfxFrameData->A = %p\n", mfxData->A);
    printf("mfxFrameData->MemId = %p\n", mfxData->MemId);
    printf("mfxFrameData->Corrupted = %d\n", mfxData->Corrupted);
    printf("mfxFrameData->DataFlag = %d\n", mfxData->DataFlag);
    //printf("mfxFrameData->BitDepthLuma = %d\n", mfxData->BitDepthLuma);
}
void printf_mfxFrameSurface1(mfxFrameSurface1 surface){
    printf_mfxFrameInfo(&surface.Info);
    printf_mfxFrameData(&surface.Data);
}

void printf_mfxBitstream(mfxBitstream *bs){
    printf("bs->DecodeTimeStamp = %ld\n", (long)bs->DecodeTimeStamp);
    printf("bs->TimeStamp = %ld\n", (long)bs->TimeStamp);
    printf("bs->Data = %p\n", bs->Data);
    printf("bs->DataOffset = %d\n", bs->DataOffset);
    printf("bs->DataLength = %d\n", bs->DataLength);
    printf("bs->MaxLength = %d\n", bs->MaxLength);
    printf("bs->PicStruct = %d\n", bs->PicStruct);
    printf("bs->FrameType = %d\n", bs->FrameType);
    printf("bs->DataFlag = %d\n", bs->DataFlag);
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