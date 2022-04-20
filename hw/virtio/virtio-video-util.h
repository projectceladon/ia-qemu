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
#ifndef QEMU_VIRTIO_VIDEO_UTIL_H
#define QEMU_VIRTIO_VIDEO_UTIL_H

#include "hw/virtio/virtio-video.h"

const char *virtio_video_cmd_name(uint32_t cmd);
const char *virtio_video_format_name(uint32_t format);
const char *virtio_video_frame_type_name(uint32_t frame_type);
int virtio_video_format_profile_range(uint32_t format,
                                      uint32_t *min, uint32_t *max);
int virtio_video_format_level_range(uint32_t format,
                                    uint32_t *min, uint32_t *max);
bool virtio_video_format_is_codec(uint32_t format);
bool virtio_video_format_is_valid(uint32_t format, uint32_t num_planes);
bool virtio_video_param_fixup(virtio_video_params *params);

void virtio_video_init_format(VirtIOVideoFormat *fmt, uint32_t format);
void virtio_video_destroy_resource(VirtIOVideoResource *resource,
                                   uint32_t mem_type, bool in);
void virtio_video_destroy_resource_list(VirtIOVideoStream *stream, bool in);
int virtio_video_memcpy(VirtIOVideoResource *res, uint32_t idx, void *src,
                        uint32_t size);
int virtio_video_memcpy_byline(VirtIOVideoResource *res, uint32_t idx, void *src_begin,
                        void *src_uv, uint32_t width, uint32_t height, uint32_t pitch,
                        uint32_t cp_size, uint32_t cp_height);
int virtio_video_memcpy_ARGB_byline(VirtIOVideoResource *res, void *src, uint32_t width,
                        uint32_t height, uint32_t pitch);
int virtio_video_memcpy_NV12_byline(VirtIOVideoResource *res, void *Y, void *UV,
                        uint32_t width, uint32_t height, uint32_t pitch);
int virtio_video_memcpy_NV12(VirtIOVideoResource *res, void *Y, uint32_t size_Y,
                             void *UV, uint32_t size_UV);
int virtio_video_memcpy_r(VirtIOVideoResource *pRes, uint32_t idx, void *pDst,  // Added by Shenlin 2022.2.28
                        uint32_t size); // resource -> surface

int virtio_video_event_complete(VirtIODevice *vdev, VirtIOVideoEvent *event);

void virtio_video_work_done(VirtIOVideoWork *work);
void virtio_video_inflight_cmd_done(VirtIOVideoStream *stream);
void virtio_video_inflight_cmd_cancel(VirtIOVideoStream *stream);
void virtio_video_report_event(VirtIOVideo *v, uint32_t event,
    uint32_t stream_id);
int virtio_video_memdump(VirtIOVideoResource *res, uint32_t idx, void *dst, uint32_t size);
#endif /* QEMU_VIRTIO_VIDEO_UTIL_H */
