/*
 * Virtio Video Device
 *
 * Copyright (C) 2022, Intel Corporation. All rights reserved.
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
 * Authors: Dong Yang <dong.yang@intel.com>
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "virtio-video-util.h"
#include "virtio-video-msdk.h"
#include "virtio-video-msdk-dec.h"
#include "virtio-video-msdk-util.h"
#include "virtio-video-va-allocator.h"
#include "mfx/mfxvideo.h"
#include "va/va.h"

//#define VIRTIO_VIDEO_VA_ALLOC_DEBUG 1
#if !defined VIRTIO_VIDEO_VA_ALLOC_DEBUG && !defined DEBUG_VIRTIO_VIDEO_ALL
#undef DPRINTF
#define DPRINTF(fmt, ...) do { } while (0)
#endif

static mfxStatus va_to_mfx_status(VAStatus va_res)
{
    mfxStatus mfxRes = MFX_ERR_NONE;

    switch (va_res) {
    case VA_STATUS_SUCCESS:
        mfxRes = MFX_ERR_NONE;
        break;
    case VA_STATUS_ERROR_ALLOCATION_FAILED:
        mfxRes = MFX_ERR_MEMORY_ALLOC;
        break;
    case VA_STATUS_ERROR_ATTR_NOT_SUPPORTED:
    case VA_STATUS_ERROR_UNSUPPORTED_PROFILE:
    case VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT:
    case VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT:
    case VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE:
    case VA_STATUS_ERROR_FLAG_NOT_SUPPORTED:
    case VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED:
        mfxRes = MFX_ERR_UNSUPPORTED;
        break;
    case VA_STATUS_ERROR_INVALID_DISPLAY:
    case VA_STATUS_ERROR_INVALID_CONFIG:
    case VA_STATUS_ERROR_INVALID_CONTEXT:
    case VA_STATUS_ERROR_INVALID_SURFACE:
    case VA_STATUS_ERROR_INVALID_BUFFER:
    case VA_STATUS_ERROR_INVALID_IMAGE:
    case VA_STATUS_ERROR_INVALID_SUBPICTURE:
        mfxRes = MFX_ERR_NOT_INITIALIZED;
        break;
    case VA_STATUS_ERROR_INVALID_PARAMETER:
        mfxRes = MFX_ERR_INVALID_VIDEO_PARAM;
        break;
    default:
        mfxRes = MFX_ERR_UNKNOWN;
        break;
    }
    return mfxRes;
}

mfxStatus virtio_video_frame_alloc(mfxHDL pthis, mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
    MsdkSurface *vv_surface;
    MsdkSession *m_session;
    int err, i;

    DPRINTF("\n");

    m_session = pthis;
    if (!(request->Type & MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET)) {
        fprintf(stderr, "Unsupported surface type: %d\n", request->Type);
        return MFX_ERR_UNSUPPORTED;
    }
    if (request->Info.BitDepthLuma != 8 || request->Info.BitDepthChroma != 8 ||
        request->Info.Shift || request->Info.FourCC != MFX_FOURCC_NV12 ||
        request->Info.ChromaFormat != MFX_CHROMAFORMAT_YUV420) {
        fprintf(stderr, "Unsupported surface properties.\n");
        return MFX_ERR_UNSUPPORTED;
    }

    m_session->surfaces = g_new0(VASurfaceID, request->NumFrameSuggested);
    m_session->surface_ids = g_new0(mfxMemId, request->NumFrameSuggested);
    m_session->surface_num = request->NumFrameSuggested;

    for (i = 0; i < request->NumFrameSuggested; i++)
    {
        vv_surface = g_new0(MsdkSurface, 1);

        vv_surface->used = false;

        err = vaCreateSurfaces(m_session->va_dpy, VA_RT_FORMAT_YUV420,
                               request->Info.Width, request->Info.Height,
                               &m_session->surfaces[i], 1,
                               NULL, 0);
        if (err != VA_STATUS_SUCCESS)
        {
            fprintf(stderr, "Error allocating VA surfaces\n");
            g_free(vv_surface);
            goto fail;
        }
        m_session->surface_ids[i] = &m_session->surfaces[i];
        // vv_surface->surface.Data.MemType = MFX_MEMTYPE_EXTERNAL_FRAME;
        vv_surface->surface.Data.MemId = &m_session->surfaces[i];

        vv_surface->surface.Data.TimeStamp = 0;
        vv_surface->surface.Data.FrameOrder = 0;

        vv_surface->surface.Info = request->Info;
        vv_surface->surface.Info.CropW = request->Info.Width;
        vv_surface->surface.Info.CropH = request->Info.Height;
        vv_surface->surface.Info.CropX = 0;
        vv_surface->surface.Info.CropW = 0;
        vv_surface->surface.Info.FourCC = request->Info.FourCC;

        DPRINTF("%s insert surface_pool:%p\n", __func__, vv_surface);
        QLIST_INSERT_HEAD(&m_session->surface_pool, vv_surface, next);
    }

    response->mids           = m_session->surface_ids;
    response->NumFrameActual = m_session->surface_num;

    return MFX_ERR_NONE;
fail:
    g_free(m_session->surfaces);
    g_free(m_session->surface_ids);
    return MFX_ERR_MEMORY_ALLOC;
}

mfxStatus virtio_video_frame_free(mfxHDL pthis, mfxFrameAllocResponse *response)
{
    MsdkSession *m_session = pthis;
    DPRINTF("\n");

    //m_session = pthis;
    if (m_session && m_session->surfaces) {
        g_free(m_session->surfaces);
        m_session->surfaces = NULL;
        DPRINTF("free m_session->surfaces\n");
    }

    if (m_session && m_session->surface_ids) {
        g_free(m_session->surface_ids);
        m_session->surface_ids = NULL;
        DPRINTF("free m_session->surface_ids\n");
    }

    return MFX_ERR_NONE;
}

enum {
    MFX_FOURCC_VP8_NV12    = MFX_MAKEFOURCC('V','P','8','N'),
    MFX_FOURCC_VP8_MBDATA  = MFX_MAKEFOURCC('V','P','8','M'),
    MFX_FOURCC_VP8_SEGMAP  = MFX_MAKEFOURCC('V','P','8','S'),
};

static unsigned int ConvertVP8FourccToMfxFourcc(mfxU32 fourcc)
{
    switch (fourcc)
    {
    case MFX_FOURCC_VP8_NV12:
    case MFX_FOURCC_VP8_MBDATA:
        return MFX_FOURCC_NV12;
    case MFX_FOURCC_VP8_SEGMAP:
        return MFX_FOURCC_P8;

    default:
        return fourcc;
    }
}

mfxStatus virtio_video_frame_lock(mfxHDL pthis, mfxMemId mid,
                                  mfxFrameData *ptr)
{
    mfxStatus mfx_res = MFX_ERR_NONE;
    VAStatus va_res = VA_STATUS_SUCCESS;
    vaapiMemId *vaapi_mid = (vaapiMemId *)mid;
    mfxU8 *pBuffer = 0;
    MsdkSession *session = pthis;

    DPRINTF("\n");

    if (!vaapi_mid)
        return MFX_ERR_INVALID_HANDLE;

    vaapi_mid->m_surface = (VASurfaceID *)(vaapi_mid->m_frame->Data.MemId);
    vaapi_mid->m_fourcc = vaapi_mid->m_frame->Info.FourCC;

    mfxU32 mfx_fourcc = ConvertVP8FourccToMfxFourcc(vaapi_mid->m_fourcc);

    if (MFX_FOURCC_P8 == mfx_fourcc) // bitstream processing
    {
        VACodedBufferSegment *coded_buffer_segment;
        if (vaapi_mid->m_fourcc == MFX_FOURCC_VP8_SEGMAP)
            va_res = vaMapBuffer(session->va_dpy, *(vaapi_mid->m_surface),
                                 (void **)(&pBuffer));
        else
            va_res = vaMapBuffer(session->va_dpy, *(vaapi_mid->m_surface),
                                 (void **)(&coded_buffer_segment));
        mfx_res = va_to_mfx_status(va_res);
        if (MFX_ERR_NONE == mfx_res) {
            if (vaapi_mid->m_fourcc == MFX_FOURCC_VP8_SEGMAP)
                ptr->Y = pBuffer;
            else
                ptr->Y = (mfxU8 *)coded_buffer_segment->buf;
        }
    } else // Image processing
    {
        va_res = vaDeriveImage(
            session->va_dpy, *(vaapi_mid->m_surface), &(vaapi_mid->m_image));
        mfx_res = va_to_mfx_status(va_res);

        if (MFX_ERR_NONE == mfx_res) {
            va_res = vaMapBuffer(
                session->va_dpy, vaapi_mid->m_image.buf, (void **)&pBuffer);
            mfx_res = va_to_mfx_status(va_res);
        }
        if (MFX_ERR_NONE == mfx_res) {
            switch (vaapi_mid->m_image.format.fourcc) {
            case VA_FOURCC_NV12:
                if (mfx_fourcc != vaapi_mid->m_image.format.fourcc)
                    return MFX_ERR_LOCK_MEMORY;

                {
                    ptr->Y = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->U = pBuffer + vaapi_mid->m_image.offsets[1];
                    ptr->V = ptr->U + 1;
                }
                break;
            case VA_FOURCC_YV12:
                if (mfx_fourcc != vaapi_mid->m_image.format.fourcc)
                    return MFX_ERR_LOCK_MEMORY;

                {
                    ptr->Y = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->V = pBuffer + vaapi_mid->m_image.offsets[1];
                    ptr->U = pBuffer + vaapi_mid->m_image.offsets[2];
                }
                break;
            case VA_FOURCC_YUY2:
                if (mfx_fourcc != vaapi_mid->m_image.format.fourcc)
                    return MFX_ERR_LOCK_MEMORY;

                {
                    ptr->Y = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->U = ptr->Y + 1;
                    ptr->V = ptr->Y + 3;
                }
                break;
            case VA_FOURCC_UYVY:
                if (mfx_fourcc != vaapi_mid->m_image.format.fourcc)
                    return MFX_ERR_LOCK_MEMORY;

                {
                    ptr->U = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->Y = ptr->U + 1;
                    ptr->V = ptr->U + 2;
                }
                break;
            case VA_FOURCC_RGB565:
                if (mfx_fourcc == MFX_FOURCC_RGB565) {
                    ptr->B = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->G = ptr->B;
                    ptr->R = ptr->B;
                } else
                    return MFX_ERR_LOCK_MEMORY;
                break;
            case VA_FOURCC_ARGB:
                if (mfx_fourcc == MFX_FOURCC_RGB4) {
                    ptr->B = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->G = ptr->B + 1;
                    ptr->R = ptr->B + 2;
                    ptr->A = ptr->B + 3;
                } else
                    return MFX_ERR_LOCK_MEMORY;
                break;
#ifndef ANDROID
            case VA_FOURCC_A2R10G10B10:
                if (mfx_fourcc == MFX_FOURCC_A2RGB10) {
                    ptr->B = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->G = ptr->B;
                    ptr->R = ptr->B;
                    ptr->A = ptr->B;
                } else
                    return MFX_ERR_LOCK_MEMORY;
                break;
#endif
            case VA_FOURCC_ABGR:
                if (mfx_fourcc == MFX_FOURCC_BGR4) {
                    ptr->R = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->G = pBuffer + vaapi_mid->m_image.offsets[1];
                    ptr->B = pBuffer + vaapi_mid->m_image.offsets[2];
                    ptr->A = ptr->R + 3;
                } else
                    return MFX_ERR_LOCK_MEMORY;
                break;
            case VA_FOURCC_RGBP:
                if (mfx_fourcc != vaapi_mid->m_image.format.fourcc)
                    return MFX_ERR_LOCK_MEMORY;

                {
                    ptr->B = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->G = pBuffer + vaapi_mid->m_image.offsets[1];
                    ptr->R = pBuffer + vaapi_mid->m_image.offsets[2];
                }
                break;
            case VA_FOURCC_P208:
                if (mfx_fourcc == MFX_FOURCC_NV12) {
                    ptr->Y = pBuffer + vaapi_mid->m_image.offsets[0];
                } else
                    return MFX_ERR_LOCK_MEMORY;
                break;
            case VA_FOURCC_P010:
            case VA_FOURCC_P016:
                if (mfx_fourcc != vaapi_mid->m_image.format.fourcc)
                    return MFX_ERR_LOCK_MEMORY;

                {
                    ptr->Y16 =
                        (mfxU16 *)(pBuffer + vaapi_mid->m_image.offsets[0]);
                    ptr->U16 =
                        (mfxU16 *)(pBuffer + vaapi_mid->m_image.offsets[1]);
                    ptr->V16 = ptr->U16 + 1;
                }
                break;
            case VA_FOURCC_AYUV:
                if (mfx_fourcc != vaapi_mid->m_image.format.fourcc)
                    return MFX_ERR_LOCK_MEMORY;

                {
                    ptr->V = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->U = ptr->V + 1;
                    ptr->Y = ptr->V + 2;
                    ptr->A = ptr->V + 3;
                }
                break;
            case VA_FOURCC_Y210:
            case VA_FOURCC_Y216:
                if (mfx_fourcc != vaapi_mid->m_image.format.fourcc)
                    return MFX_ERR_LOCK_MEMORY;

                {
                    ptr->Y16 =
                        (mfxU16 *)(pBuffer + vaapi_mid->m_image.offsets[0]);
                    ptr->U16 = ptr->Y16 + 1;
                    ptr->V16 = ptr->Y16 + 3;
                }
                break;
            case VA_FOURCC_Y410:
                if (mfx_fourcc != vaapi_mid->m_image.format.fourcc)
                    return MFX_ERR_LOCK_MEMORY;

                {
                    ptr->Y410 =
                        (mfxY410 *)(pBuffer + vaapi_mid->m_image.offsets[0]);
                    ptr->Y = 0;
                    ptr->V = 0;
                    ptr->A = 0;
                }
                break;
            case VA_FOURCC_Y416:
                if (mfx_fourcc != vaapi_mid->m_image.format.fourcc)
                    return MFX_ERR_LOCK_MEMORY;

                {
                    ptr->U16 =
                        (mfxU16 *)(pBuffer + vaapi_mid->m_image.offsets[0]);
                    ptr->Y16 = ptr->U16 + 1;
                    ptr->V16 = ptr->Y16 + 1;
                    ptr->A = (mfxU8 *)(ptr->V16 + 1);
                }
                break;
            default:
                return MFX_ERR_LOCK_MEMORY;
            }
        }

        ptr->PitchHigh = (mfxU16)(vaapi_mid->m_image.pitches[0] / (1 << 16));
        ptr->PitchLow = (mfxU16)(vaapi_mid->m_image.pitches[0] % (1 << 16));
    }
    return mfx_res;
}

mfxStatus virtio_video_frame_unlock(mfxHDL pthis, mfxMemId mid,
                                    mfxFrameData *ptr)
{
    vaapiMemId *vaapi_mid = (vaapiMemId *)mid;
    MsdkSession *session = pthis;

    DPRINTF("\n");
    if (!vaapi_mid || !(vaapi_mid->m_surface))
        return MFX_ERR_INVALID_HANDLE;

    mfxU32 mfx_fourcc = ConvertVP8FourccToMfxFourcc(vaapi_mid->m_fourcc);

    if (MFX_FOURCC_P8 == mfx_fourcc) // bitstream processing
    {
        vaUnmapBuffer(session->va_dpy, *(vaapi_mid->m_surface));
    } else // Image processing
    {
        vaUnmapBuffer(session->va_dpy, vaapi_mid->m_image.buf);
        vaDestroyImage(session->va_dpy, vaapi_mid->m_image.image_id);

        if (NULL != ptr) {
            ptr->PitchLow = 0;
            ptr->PitchHigh = 0;
            ptr->Y = NULL;
            ptr->U = NULL;
            ptr->V = NULL;
            ptr->A = NULL;
        }
    }
    return MFX_ERR_NONE;
}

mfxStatus virtio_video_frame_get_handle(mfxHDL pthis, mfxMemId mid, mfxHDL *handle)
{
    DPRINTF("\n");
    *handle = mid;
    return MFX_ERR_NONE;
}
