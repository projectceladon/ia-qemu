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
#ifndef QEMU_VIRTIO_VIDEO_MSDK_UTIL_H
#define QEMU_VIRTIO_VIDEO_MSDK_UTIL_H

#include "mfx/mfxsession.h"
#include "mfx/mfxstructures.h"
#include "hw/virtio/virtio-video.h"
#include "hw/virtio/virtio-video-msdk.h"

#define MSDK_ALIGN16(x) (((x + 15) >> 4) << 4)
#define MSDK_ALIGN32(x) (((x + 31) >> 5) << 5)

uint32_t virtio_video_format_to_msdk(uint32_t format);
uint32_t virtio_video_profile_to_msdk(uint32_t profile);
uint32_t virtio_video_level_to_msdk(uint32_t level);
uint32_t virtio_video_msdk_to_profile(uint32_t msdk);
uint32_t virtio_video_msdk_to_level(uint32_t msdk);
uint32_t virtio_video_msdk_to_frame_type(uint32_t msdk);
uint32_t virtio_video_get_frame_type(mfxU16 m_frameType);

int virtio_video_msdk_init_param(mfxVideoParam *param, uint32_t format);
int virtio_video_msdk_init_param_dec(MsdkSession *session, mfxVideoParam *param,
                                     VirtIOVideoStream *stream);
int virtio_video_msdk_init_vpp_param_dec(mfxVideoParam *param,
    mfxVideoParam *vpp_param, VirtIOVideoStream *stream);
void *virtio_video_msdk_inc_pool_size(MsdkSession *session, uint32_t inc_num, bool vpp, bool encode); // Added by Shenlin
void virtio_video_msdk_init_surface_pool(MsdkSession *session,
    mfxFrameAllocRequest *alloc_req, mfxFrameInfo *info, bool vpp, bool encode);
void virtio_video_msdk_uninit_surface_pools(MsdkSession *session);
void virtio_video_msdk_uninit_frame(VirtIOVideoFrame *frame);
int virtio_video_msdk_input_surface(MsdkSurface *surface, VirtIOVideoResource *resource); // Added by Shenlin
int virtio_video_msdk_output_surface(MsdkSession *session, MsdkSurface *surface, VirtIOVideoResource *resource);
void virtio_video_msdk_stream_reset_param(VirtIOVideoStream *stream,
    mfxVideoParam *param, bool encode);

void virtio_video_msdk_load_plugin(mfxSession session, uint32_t format,
    bool encode);
void virtio_video_msdk_unload_plugin(mfxSession session, uint32_t format,
    bool encode);
int virtio_video_msdk_init_handle(VirtIOVideo *vdev);
void virtio_video_msdk_uninit_handle(VirtIOVideo *vdev);

void printf_mfxVideoParam(mfxVideoParam *mfxVideoParam);
void printf_mfxFrameInfo(mfxFrameInfo *mfxInfo);
void printf_mfxFrameData(mfxFrameData *mfxData);
void printf_mfxFrameSurface1(mfxFrameSurface1 *surface);
void printf_mfxBitstream(mfxBitstream *bs);

char * virtio_video_fmt_to_string(virtio_video_format fmt);
char * virtio_video_status_to_string(mfxStatus fmt);
void virtio_video_msdk_dump_surface(char * src, int len);
char * virtio_video_stream_statu_to_string(virtio_video_stream_state statu);

// added by shenlin
void virtio_video_msdk_get_preset_param_enc(VirtIOVideoEncodeParamPreset *pPp, uint32_t fourCC, double fps, uint32_t width, uint32_t height);
int virtio_video_msdk_convert_frame_rate(double frame_rate, uint32_t *pRateExtN, uint32_t *pRateExtD);
uint16_t virtio_video_msdk_fourCC_to_chroma(uint32_t fourCC);
void virtio_video_msdk_add_pf_to_pool(MsdkSession *session, uint32_t buffer_size, uint32_t inc_num);
// end

#endif /* QEMU_VIRTIO_VIDEO_MSDK_UTIL_H */
