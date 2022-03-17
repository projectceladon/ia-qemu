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
            goto fail;
        }
        m_session->surface_ids[i] = &m_session->surfaces[i];
        // vv_surface->surface.Data.MemType = MFX_MEMTYPE_EXTERNAL_FRAME; //?
        vv_surface->surface.Data.MemId = &m_session->surfaces[i];

        vv_surface->surface.Data.TimeStamp = 0;  // what?
        vv_surface->surface.Data.FrameOrder = 0; // what?

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
    MsdkSession *m_session;
    DPRINTF("\n");

    m_session = pthis;
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

mfxStatus virtio_video_frame_lock(mfxHDL pthis, mfxMemId mid, mfxFrameData *frame_data)
{
    DPRINTF("\n");
    return MFX_ERR_NONE;
}

mfxStatus virtio_video_frame_unlock(mfxHDL pthis, mfxMemId mid, mfxFrameData *frame_data)
{
    DPRINTF("\n");
    return MFX_ERR_NONE;
}

mfxStatus virtio_video_frame_get_handle(mfxHDL pthis, mfxMemId mid, mfxHDL *handle)
{
    DPRINTF("\n");
    *handle = mid;
    return MFX_ERR_NONE;
}