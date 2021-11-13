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
 * Authors: Colin Xu <colin.xu@intel.com>
 *          Zhuocheng Ding <zhuocheng.ding@intel.com>
 */
#include "qemu/osdep.h"
#include "virtio-video-msdk-util.h"
#include "virtio-video-msdk-vaapi.h"
#include "va/va.h"
#include "va/va_drm.h"

int virtio_video_create_va_env_drm(VirtIODevice *vdev)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoMediaSDK *msdk = g_malloc(sizeof(VirtIOVideoMediaSDK));
    VAStatus va_status;
    int ver_major, ver_minor;

    msdk->drm_fd = open(VIRTIO_VIDEO_DRM_DEVICE, O_RDWR);
    if (msdk->drm_fd < 0) {
        VIRTVID_ERROR("error open DRM_DEVICE %s\n", VIRTIO_VIDEO_DRM_DEVICE);
        g_free(msdk);
        return -1;
    }

    msdk->va_disp_handle = vaGetDisplayDRM(msdk->drm_fd);
    if (!msdk->va_disp_handle) {
        VIRTVID_ERROR("error vaGetDisplayDRM for %s\n", VIRTIO_VIDEO_DRM_DEVICE);
        close(msdk->drm_fd);
        g_free(msdk);
        return -1;
    }

    va_status = vaInitialize(msdk->va_disp_handle, &ver_major, &ver_minor);
    if (va_status != VA_STATUS_SUCCESS) {
        VIRTVID_ERROR("error vaInitialize for %s, status %d\n", VIRTIO_VIDEO_DRM_DEVICE, va_status);
        vaTerminate(msdk->va_disp_handle);
        close(msdk->drm_fd);
        g_free(msdk);
        return -1;
    }

    v->opaque = msdk;
    return 0;
}

void virtio_video_destroy_va_env_drm(VirtIODevice *vdev)
{
    VirtIOVideo *v = VIRTIO_VIDEO(vdev);
    VirtIOVideoMediaSDK *msdk = (VirtIOVideoMediaSDK *) v->opaque;

    if (msdk->va_disp_handle) {
        vaTerminate(msdk->va_disp_handle);
        msdk->va_disp_handle = NULL;
    }

    if (msdk->drm_fd) {
        close(msdk->drm_fd);
        msdk->drm_fd = 0;
    }

    g_free(msdk);
    v->opaque = NULL;
}
