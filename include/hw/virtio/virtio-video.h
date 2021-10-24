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
#ifndef QEMU_VIRTIO_VIDEO_H
#define QEMU_VIRTIO_VIDEO_H

#include "standard-headers/linux/virtio_video.h"
#include "hw/virtio/virtio.h"
#include "qemu/timer.h"

#define VIRTIO_VIDEO_DEBUG

#define VIRTIO_VIDEO_DEBUG_LEVEL_QUIET 0
#define VIRTIO_VIDEO_DEBUG_LEVEL_ERROR 1
#define VIRTIO_VIDEO_DEBUG_LEVEL_WARN 2
#define VIRTIO_VIDEO_DEBUG_LEVEL_DEBUG 3
#define VIRTIO_VIDEO_DEBUG_LEVEL_VERBOSE 4
#define VIRTIO_VIDEO_DEBUG_DEVEL VIRTIO_VIDEO_DEBUG_LEVEL_DEBUG

#ifdef VIRTIO_VIDEO_DEBUG
#define VIRTVID_PRINT(lvl, fmt, ...) do { \
        if (lvl <= VIRTIO_VIDEO_DEBUG_DEVEL) \
        printf("[%ld]virtio-video: " fmt"\n" , get_clock(), ## __VA_ARGS__); \
    } while (0)
#define VIRTVID_ERROR(fmt, ...) VIRTVID_PRINT(VIRTIO_VIDEO_DEBUG_LEVEL_ERROR, fmt, ## __VA_ARGS__)
#define VIRTVID_WARN(fmt, ...) VIRTVID_PRINT(VIRTIO_VIDEO_DEBUG_LEVEL_WARN, fmt, ## __VA_ARGS__)
#define VIRTVID_DEBUG(fmt, ...) VIRTVID_PRINT(VIRTIO_VIDEO_DEBUG_LEVEL_DEBUG, fmt, ## __VA_ARGS__)
#define VIRTVID_VERBOSE(fmt, ...) VIRTVID_PRINT(VIRTIO_VIDEO_DEBUG_LEVEL_VERBOSE, fmt, ## __VA_ARGS__)
#else
#define VIRTVID_ERROR(fmt, ...) do { } while (0)
#define VIRTVID_WARN(fmt, ...) do { } while (0)
#define VIRTVID_DEBUG(fmt, ...) do { } while (0)
#define VIRTVID_VERBOSE(fmt, ...) do { } while (0)
#endif

#define TYPE_VIRTIO_VIDEO "virtio-video-device"

#define VIRTIO_VIDEO_VQ_SIZE 256

#define VIRTIO_VIDEO_VERSION 0
#define VIRTIO_VIDEO_CAPS_LENGTH_MAX 1024
#define VIRTIO_VIDEO_RESPONSE_LENGTH_MAX 1024

#define VIRTIO_VIDEO(obj) \
        OBJECT_CHECK(VirtIOVideo, (obj), TYPE_VIRTIO_VIDEO)

typedef enum virtio_video_device_model {
    VIRTIO_VIDEO_DEVICE_MIN = 1,
    VIRTIO_VIDEO_DEVICE_V4L2_DEC = VIRTIO_VIDEO_DEVICE_MIN,
    VIRTIO_VIDEO_DEVICE_V4L2_ENC,
    VIRTIO_VIDEO_DEVICE_MAX = VIRTIO_VIDEO_DEVICE_V4L2_ENC,
} virtio_video_device_model;

typedef enum virtio_video_backend {
    VIRTIO_VIDEO_BACKEND_MIN = 1,
    VIRTIO_VIDEO_BACKEND_VAAPI = VIRTIO_VIDEO_BACKEND_MIN,
    VIRTIO_VIDEO_BACKEND_FFMPEG,
    VIRTIO_VIDEO_BACKEND_GSTREAMER,
	VIRTIO_VIDEO_BACKEND_MEDIA_SDK,
    VIRTIO_VIDEO_BACKEND_MAX = VIRTIO_VIDEO_BACKEND_MEDIA_SDK,
} virtio_video_backend;

typedef struct VirtIOVideoControl {
    uint32_t value;
    QLIST_ENTRY(VirtIOVideoControl) next;
} VirtIOVideoControl;

typedef enum VirtIOVideoStreamEvent {
    VirtIOVideoStreamEventNone = 0,
    VirtIOVideoStreamEventParamChange,
    VirtIOVideoStreamEventStreamDrain,
    VirtIOVideoStreamEventResourceQueue,
    VirtIOVideoStreamEventQueueClear,
    VirtIOVideoStreamEventTerminate,
} VirtIOVideoStreamEvent;

typedef enum VirtIOVideoStreamStat {
    VirtIOVideoStreamStatNone = 0,
    VirtIOVideoStreamStatError,
    VirtIOVideoStreamStatEndOfStream,
} VirtIOVideoStreamStat;

typedef struct VirtIOVideoStreamEventEntry {
    VirtIOVideoStreamEvent ev;
    void *data;
    QLIST_ENTRY(VirtIOVideoStreamEventEntry) next;
} VirtIOVideoStreamEventEntry;

typedef struct VirtIOVideoResourceDesc {
    union {
        virtio_video_mem_entry mem_entry;
        virtio_video_object_entry obj_entry;
    } entry;
    void *hva;
    uint32_t len;
    MemoryRegion *mr;
} VirtIOVideoResourceDesc;

typedef struct VirtIOVideoStreamResource {
    uint32_t resource_id;
    virtio_video_mem_type mem_type;
    uint32_t planes_layout;
    uint32_t num_planes;
    __le32 plane_offsets[VIRTIO_VIDEO_MAX_PLANES];
    __le32 num_entries[VIRTIO_VIDEO_MAX_PLANES];
    VirtIOVideoResourceDesc *desc[VIRTIO_VIDEO_MAX_PLANES];
    QLIST_ENTRY(VirtIOVideoStreamResource) next;
} VirtIOVideoStreamResource;

typedef struct VirtIOVideoStream {
    void *mfx_session;
    uint32_t mfxWaitMs;
    uint32_t retry;
    uint32_t stream_id;
    virtio_video_mem_type in_mem_type;
    virtio_video_mem_type out_mem_type;
    virtio_video_format in_format;
    struct {
        struct {
            uint32_t num;
            QLIST_HEAD(, VirtIOVideoControl) list;
        } profile;
        struct {
            uint32_t num;
            QLIST_HEAD(, VirtIOVideoControl) list;
        } level;
    } control_caps;
    struct {
        uint32_t bitrate;
        uint32_t profile;
        uint32_t level;
    } control;
    QLIST_HEAD(, VirtIOVideoStreamEventEntry) ev_list;
    QLIST_HEAD(, VirtIOVideoStreamResource) in_list;
    QLIST_HEAD(, VirtIOVideoStreamResource) out_list;
    void *mfxParams;
    void *mfxBs;
    void *mfxSurfOut;
    QemuEvent signal_in;
    QemuEvent signal_out;
    VirtIOVideoStreamStat stat;
    virtio_video_params in_params;
    virtio_video_params out_params;
    char tag[64];
    QemuThread thread;
    QemuMutex mutex;
    VirtQueue *event_vq;
    QLIST_ENTRY(VirtIOVideoStream) next;
} VirtIOVideoStream;

typedef struct VirtIOVideoCaps {
    void *ptr;
    uint32_t size;
} VirtIOVideoCaps;

typedef struct VirtIOVideoConf {
    char *model;
    char *backend;
} VirtIOVideoConf;

typedef struct VirtIOVideoEvent {
    VirtQueueElement *elem;
    QLIST_ENTRY(VirtIOVideoEvent) next;
} VirtIOVideoEvent;

typedef struct VirtIOVideo {
    VirtIODevice parent_obj;
    VirtIOVideoConf conf;
    virtio_video_device_model model;
    virtio_video_backend backend;
    virtio_video_config config;
    VirtQueue *cmd_vq, *event_vq;
    QLIST_HEAD(, VirtIOVideoEvent) event_list;
    QLIST_HEAD(, VirtIOVideoStream) stream_list;
    void *opaque;
    VirtIOVideoCaps caps_in;
    VirtIOVideoCaps caps_out;
} VirtIOVideo;

void virtio_video_resource_desc_from_guest_page(VirtIOVideoResourceDesc *desc);

#endif /* QEMU_VIRTIO_VIDEO_H */
