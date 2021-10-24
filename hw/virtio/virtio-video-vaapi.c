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
#include "virtio-video-msdk.h"
#include "virtio-video-vaapi.h"
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

void virtio_video_vaapi_query_caps(virtio_video_format fmt)
{
    int drm_fd = -1;
    VADisplay va_dpy;
    VAStatus va_status;
    int ver_major, ver_minor;

    drm_fd = open(VIRTIO_VIDEO_DRM_DEVICE, O_RDWR);
    if (drm_fd < 0)
        printf("error open %s\n", VIRTIO_VIDEO_DRM_DEVICE);

    va_dpy = vaGetDisplayDRM(drm_fd);
    if (va_dpy) {
        int num_entrypoint = 0, num_profiles = 0, i;
        VAProfile profile, *profile_list = NULL;

        va_status = vaInitialize(va_dpy, &ver_major, &ver_minor);
        printf("%s: VA-API version: %d.%d, status %d\n",
               __FUNCTION__, ver_major, ver_minor, va_status);

        num_entrypoint = vaMaxNumEntrypoints (va_dpy);
        num_profiles = vaMaxNumProfiles(va_dpy);
        printf("%s: num_entrypoint %d, num_profiles %d\n", __FUNCTION__, num_entrypoint, num_profiles);

        profile_list = malloc(num_profiles * sizeof(VAProfile));

        va_status = vaQueryConfigProfiles(va_dpy, profile_list, &num_profiles);
        printf("%s: vaQueryConfigProfiles num_profiles %d, returns %d\n", __FUNCTION__, num_profiles, va_status);

        for (i = 0; i < num_profiles; i++) {
            profile = profile_list[i];
            printf("Profile %d\n", profile);
        }

        free(profile_list);
        vaTerminate(va_dpy);
    } else {
        printf("error open VADisplay for %s\n", VIRTIO_VIDEO_DRM_DEVICE);
    }

    if (drm_fd < 0)
        return;
    close(drm_fd);
}
