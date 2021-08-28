/*
 * VirtIO-Video Backend Driver
 * VirtIO-Video Backend defines
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
 * Author: Colin Xu <Colin.Xu@intel.com>
 *
 */
#ifndef QEMU_VIRTIO_VIDEO_H
#define QEMU_VIRTIO_VIDEO_H

#include <linux/version.h>
#include <sys/sysinfo.h>

#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "standard-headers/linux/virtio_ids.h"

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

/* Align with include/uapi/linux/virtio_video.h */
/*
 * Feature bits
 */

/* Guest pages can be used for video buffers. */
#define VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES 0
/*
 * The host can process buffers even if they are non-contiguous memory such as
 * scatter-gather lists.
 */
#define VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG 1
/* Objects exported by another virtio device can be used for video buffers */
#define VIRTIO_VIDEO_F_RESOURCE_VIRTIO_OBJECT 2

/*
 * Image formats
 */

typedef enum virtio_video_format {
    /* Raw formats */
    VIRTIO_VIDEO_FORMAT_RAW_MIN = 1,
    VIRTIO_VIDEO_FORMAT_ARGB8888 = VIRTIO_VIDEO_FORMAT_RAW_MIN,
    VIRTIO_VIDEO_FORMAT_BGRA8888,
    VIRTIO_VIDEO_FORMAT_NV12, /* 12  Y/CbCr 4:2:0  */
    VIRTIO_VIDEO_FORMAT_YUV420, /* 12  YUV 4:2:0     */
    VIRTIO_VIDEO_FORMAT_YVU420, /* 12  YVU 4:2:0     */
    VIRTIO_VIDEO_FORMAT_RAW_MAX = VIRTIO_VIDEO_FORMAT_YVU420,

    /* Coded formats */
    VIRTIO_VIDEO_FORMAT_CODED_MIN = 0x1000,
    VIRTIO_VIDEO_FORMAT_MPEG2 =
        VIRTIO_VIDEO_FORMAT_CODED_MIN, /* MPEG-2 Part 2 */
    VIRTIO_VIDEO_FORMAT_MPEG4, /* MPEG-4 Part 2 */
    VIRTIO_VIDEO_FORMAT_H264, /* H.264 */
    VIRTIO_VIDEO_FORMAT_HEVC, /* HEVC aka H.265*/
    VIRTIO_VIDEO_FORMAT_VP8, /* VP8 */
    VIRTIO_VIDEO_FORMAT_VP9, /* VP9 */
    VIRTIO_VIDEO_FORMAT_CODED_MAX = VIRTIO_VIDEO_FORMAT_VP9,
} virtio_video_format;

typedef enum virtio_video_profile {
    /* H.264 */
    VIRTIO_VIDEO_PROFILE_H264_MIN = 0x100,
    VIRTIO_VIDEO_PROFILE_H264_BASELINE = VIRTIO_VIDEO_PROFILE_H264_MIN,
    VIRTIO_VIDEO_PROFILE_H264_MAIN,
    VIRTIO_VIDEO_PROFILE_H264_EXTENDED,
    VIRTIO_VIDEO_PROFILE_H264_HIGH,
    VIRTIO_VIDEO_PROFILE_H264_HIGH10PROFILE,
    VIRTIO_VIDEO_PROFILE_H264_HIGH422PROFILE,
    VIRTIO_VIDEO_PROFILE_H264_HIGH444PREDICTIVEPROFILE,
    VIRTIO_VIDEO_PROFILE_H264_SCALABLEBASELINE,
    VIRTIO_VIDEO_PROFILE_H264_SCALABLEHIGH,
    VIRTIO_VIDEO_PROFILE_H264_STEREOHIGH,
    VIRTIO_VIDEO_PROFILE_H264_MULTIVIEWHIGH,
    VIRTIO_VIDEO_PROFILE_H264_MAX = VIRTIO_VIDEO_PROFILE_H264_MULTIVIEWHIGH,

    /* HEVC */
    VIRTIO_VIDEO_PROFILE_HEVC_MIN = 0x200,
    VIRTIO_VIDEO_PROFILE_HEVC_MAIN = VIRTIO_VIDEO_PROFILE_HEVC_MIN,
    VIRTIO_VIDEO_PROFILE_HEVC_MAIN10,
    VIRTIO_VIDEO_PROFILE_HEVC_MAIN_STILL_PICTURE,
    VIRTIO_VIDEO_PROFILE_HEVC_MAX =
        VIRTIO_VIDEO_PROFILE_HEVC_MAIN_STILL_PICTURE,

    /* VP8 */
    VIRTIO_VIDEO_PROFILE_VP8_MIN = 0x300,
    VIRTIO_VIDEO_PROFILE_VP8_PROFILE0 = VIRTIO_VIDEO_PROFILE_VP8_MIN,
    VIRTIO_VIDEO_PROFILE_VP8_PROFILE1,
    VIRTIO_VIDEO_PROFILE_VP8_PROFILE2,
    VIRTIO_VIDEO_PROFILE_VP8_PROFILE3,
    VIRTIO_VIDEO_PROFILE_VP8_MAX = VIRTIO_VIDEO_PROFILE_VP8_PROFILE3,

    /* VP9 */
    VIRTIO_VIDEO_PROFILE_VP9_MIN = 0x400,
    VIRTIO_VIDEO_PROFILE_VP9_PROFILE0 = VIRTIO_VIDEO_PROFILE_VP9_MIN,
    VIRTIO_VIDEO_PROFILE_VP9_PROFILE1,
    VIRTIO_VIDEO_PROFILE_VP9_PROFILE2,
    VIRTIO_VIDEO_PROFILE_VP9_PROFILE3,
    VIRTIO_VIDEO_PROFILE_VP9_MAX = VIRTIO_VIDEO_PROFILE_VP9_PROFILE3,

    /* MPEG2 */
    VIRTIO_VIDEO_PROFILE_MPEG2_MIN = 0x500,
    VIRTIO_VIDEO_PROFILE_MPEG2_SIMPLE = VIRTIO_VIDEO_PROFILE_MPEG2_MIN,
    VIRTIO_VIDEO_PROFILE_MPEG2_MAIN,
    VIRTIO_VIDEO_PROFILE_MPEG2_HIGH,
    VIRTIO_VIDEO_PROFILE_MPEG2_MAX = VIRTIO_VIDEO_PROFILE_MPEG2_HIGH,
} virtio_video_profile;

typedef enum virtio_video_level {
    /* H.264 */
    VIRTIO_VIDEO_LEVEL_H264_MIN = 0x100,
    VIRTIO_VIDEO_LEVEL_H264_1_0 = VIRTIO_VIDEO_LEVEL_H264_MIN,
    VIRTIO_VIDEO_LEVEL_H264_1_1,
    VIRTIO_VIDEO_LEVEL_H264_1_2,
    VIRTIO_VIDEO_LEVEL_H264_1_3,
    VIRTIO_VIDEO_LEVEL_H264_2_0,
    VIRTIO_VIDEO_LEVEL_H264_2_1,
    VIRTIO_VIDEO_LEVEL_H264_2_2,
    VIRTIO_VIDEO_LEVEL_H264_3_0,
    VIRTIO_VIDEO_LEVEL_H264_3_1,
    VIRTIO_VIDEO_LEVEL_H264_3_2,
    VIRTIO_VIDEO_LEVEL_H264_4_0,
    VIRTIO_VIDEO_LEVEL_H264_4_1,
    VIRTIO_VIDEO_LEVEL_H264_4_2,
    VIRTIO_VIDEO_LEVEL_H264_5_0,
    VIRTIO_VIDEO_LEVEL_H264_5_1,
    VIRTIO_VIDEO_LEVEL_H264_MAX = VIRTIO_VIDEO_LEVEL_H264_5_1,

    /* HEVC */
    VIRTIO_VIDEO_LEVEL_HEVC_MIN = 0x200,
    VIRTIO_VIDEO_LEVEL_HEVC_1_0 = VIRTIO_VIDEO_LEVEL_HEVC_MIN,
    VIRTIO_VIDEO_LEVEL_HEVC_2_0,
    VIRTIO_VIDEO_LEVEL_HEVC_2_1,
    VIRTIO_VIDEO_LEVEL_HEVC_3_0,
    VIRTIO_VIDEO_LEVEL_HEVC_3_1,
    VIRTIO_VIDEO_LEVEL_HEVC_4_0,
    VIRTIO_VIDEO_LEVEL_HEVC_4_1,
    VIRTIO_VIDEO_LEVEL_HEVC_5_0,
    VIRTIO_VIDEO_LEVEL_HEVC_5_1,
    VIRTIO_VIDEO_LEVEL_HEVC_5_2,
    VIRTIO_VIDEO_LEVEL_HEVC_6_0,
    VIRTIO_VIDEO_LEVEL_HEVC_6_1,
    VIRTIO_VIDEO_LEVEL_HEVC_6_2,
    VIRTIO_VIDEO_LEVEL_HEVC_MAX = VIRTIO_VIDEO_LEVEL_HEVC_6_2,

    /* VP8 */
    VIRTIO_VIDEO_LEVEL_VP8_MIN = 0x300,
    VIRTIO_VIDEO_LEVEL_VP8_MAX = VIRTIO_VIDEO_LEVEL_VP8_MIN,

    /* VP9 */
    VIRTIO_VIDEO_LEVEL_VP9_MIN = 0x400,
    VIRTIO_VIDEO_LEVEL_VP9_MAX = VIRTIO_VIDEO_LEVEL_VP9_MIN,

    /* MPEG2 */
    VIRTIO_VIDEO_LEVEL_MPEG2_MIN = 0x500,
    VIRTIO_VIDEO_LEVEL_MPEG2_LOW = VIRTIO_VIDEO_LEVEL_MPEG2_MIN,
    VIRTIO_VIDEO_LEVEL_MPEG2_MAIN,
    VIRTIO_VIDEO_LEVEL_MPEG2_HIGH,
    VIRTIO_VIDEO_LEVEL_MPEG2_HIGH_1440,
    VIRTIO_VIDEO_LEVEL_MPEG2_MAX = VIRTIO_VIDEO_LEVEL_MPEG2_HIGH_1440,
} virtio_video_level;

/*
 * Config
 */

typedef struct __attribute__ ((packed)) virtio_video_config {
    __le32 version;
    __le32 max_caps_length;
    __le32 max_resp_length;
} virtio_video_config;

/*
 * Commands
 */

typedef enum virtio_video_cmd_type {
    /* Command */
    VIRTIO_VIDEO_CMD_QUERY_CAPABILITY = 0x0100,
    VIRTIO_VIDEO_CMD_STREAM_CREATE,
    VIRTIO_VIDEO_CMD_STREAM_DESTROY,
    VIRTIO_VIDEO_CMD_STREAM_DRAIN,
    VIRTIO_VIDEO_CMD_RESOURCE_CREATE,
    VIRTIO_VIDEO_CMD_RESOURCE_QUEUE,
    VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL,
    VIRTIO_VIDEO_CMD_QUEUE_CLEAR,
    VIRTIO_VIDEO_CMD_GET_PARAMS,
    VIRTIO_VIDEO_CMD_SET_PARAMS,
    VIRTIO_VIDEO_CMD_QUERY_CONTROL,
    VIRTIO_VIDEO_CMD_GET_CONTROL,
    VIRTIO_VIDEO_CMD_SET_CONTROL,

    /* Response */
    VIRTIO_VIDEO_RESP_OK_NODATA = 0x0200,
    VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY,
    VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE,
    VIRTIO_VIDEO_RESP_OK_GET_PARAMS,
    VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL,
    VIRTIO_VIDEO_RESP_OK_GET_CONTROL,

    VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION = 0x0300,
    VIRTIO_VIDEO_RESP_ERR_OUT_OF_MEMORY,
    VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
    VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID,
    VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
    VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL,
} virtio_video_cmd_type;

typedef struct virtio_video_cmd_hdr {
    __le32 type; /* One of enum virtio_video_cmd_type */
    __le32 stream_id;
} virtio_video_cmd_hdr;

/* VIRTIO_VIDEO_CMD_QUERY_CAPABILITY */
typedef enum virtio_video_queue_type {
    VIRTIO_VIDEO_QUEUE_TYPE_INPUT = 0x100,
    VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT,
} virtio_video_queue_type;

typedef struct virtio_video_query_capability {
    struct virtio_video_cmd_hdr hdr;
    __le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
    __u8 padding[4];
} virtio_video_query_capability;

typedef enum virtio_video_planes_layout_flag {
    VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER = 1 << 0,
    VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE = 1 << 1,
} virtio_video_planes_layout_flag;

typedef struct virtio_video_format_range {
    __le32 min;
    __le32 max;
    __le32 step;
    __u8 padding[4];
} virtio_video_format_range;

typedef struct virtio_video_format_frame {
    struct virtio_video_format_range width;
    struct virtio_video_format_range height;
    __le32 num_rates;
    __u8 padding[4];
    /* Followed by struct virtio_video_format_range frame_rates[] */
} virtio_video_format_frame;

typedef struct virtio_video_format_desc {
    __le64 mask;
    __le32 format; /* One of VIRTIO_VIDEO_FORMAT_* types */
    __le32 planes_layout; /* Bitmask with VIRTIO_VIDEO_PLANES_LAYOUT_* */
    __le32 plane_align;
    __le32 num_frames;
    /* Followed by struct virtio_video_format_frame frames[] */
} virtio_video_format_desc;

typedef struct virtio_video_query_capability_resp {
    struct virtio_video_cmd_hdr hdr;
    __le32 num_descs;
    __u8 padding[4];
    /* Followed by struct virtio_video_format_desc descs[] */
} virtio_video_query_capability_resp;

/* VIRTIO_VIDEO_CMD_STREAM_CREATE */
typedef enum virtio_video_mem_type {
    VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES,
    VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT,
} virtio_video_mem_type;

typedef struct virtio_video_stream_create {
    struct virtio_video_cmd_hdr hdr;
    __le32 in_mem_type; /* One of VIRTIO_VIDEO_MEM_TYPE_* types */
    __le32 out_mem_type; /* One of VIRTIO_VIDEO_MEM_TYPE_* types */
    __le32 coded_format; /* One of VIRTIO_VIDEO_FORMAT_* types */
    __u8 padding[4];
    __u8 tag[64];
} virtio_video_stream_create;

/* VIRTIO_VIDEO_CMD_STREAM_DESTROY */
typedef struct virtio_video_stream_destroy {
    struct virtio_video_cmd_hdr hdr;
} virtio_video_stream_destroy;

/* VIRTIO_VIDEO_CMD_STREAM_DRAIN */
typedef struct virtio_video_stream_drain {
    struct virtio_video_cmd_hdr hdr;
} virtio_video_stream_drain;

/* VIRTIO_VIDEO_CMD_RESOURCE_CREATE */
typedef struct virtio_video_mem_entry {
    __le64 addr;
    __le32 length;
    __u8 padding[4];
} virtio_video_mem_entry;

typedef struct virtio_video_object_entry {
    __u8 uuid[16];
} virtio_video_object_entry;

#define VIRTIO_VIDEO_MAX_PLANES 8

typedef struct virtio_video_resource_create {
    struct virtio_video_cmd_hdr hdr;
    __le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
    __le32 resource_id;
    __le32 planes_layout;
    __le32 num_planes;
    __le32 plane_offsets[VIRTIO_VIDEO_MAX_PLANES];
    __le32 num_entries[VIRTIO_VIDEO_MAX_PLANES];
    /**
        * Followed by either
        * - struct virtio_video_mem_entry entries[]
        *   for VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES
        * - struct virtio_video_object_entry entries[]
        *   for VIRTIO_VIDEO_MEM_TYPE_VIRTIO_OBJECT
        */
} virtio_video_resource_create;

/* VIRTIO_VIDEO_CMD_RESOURCE_QUEUE */
typedef struct virtio_video_resource_queue {
    struct virtio_video_cmd_hdr hdr;
    __le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
    __le32 resource_id;
    __le64 timestamp;
    __le32 num_data_sizes;
    __le32 data_sizes[VIRTIO_VIDEO_MAX_PLANES];
    __u8 padding[4];
} virtio_video_resource_queue;

typedef enum virtio_video_buffer_flag {
    VIRTIO_VIDEO_BUFFER_FLAG_ERR = 0x0001,
    VIRTIO_VIDEO_BUFFER_FLAG_EOS = 0x0002,

    /* Encoder only */
    VIRTIO_VIDEO_BUFFER_FLAG_IFRAME = 0x0004,
    VIRTIO_VIDEO_BUFFER_FLAG_PFRAME = 0x0008,
    VIRTIO_VIDEO_BUFFER_FLAG_BFRAME = 0x0010,
} virtio_video_buffer_flag;

typedef struct virtio_video_resource_queue_resp {
    struct virtio_video_cmd_hdr hdr;
    __le64 timestamp;
    __le32 flags; /* One of VIRTIO_VIDEO_BUFFER_FLAG_* flags */
    __le32 size; /* Encoded size */
} virtio_video_resource_queue_resp;

/* VIRTIO_VIDEO_CMD_RESOURCE_DESTROY_ALL */
typedef struct virtio_video_resource_destroy_all {
    struct virtio_video_cmd_hdr hdr;
    __le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
    __u8 padding[4];
} virtio_video_resource_destroy_all;

/* VIRTIO_VIDEO_CMD_QUEUE_CLEAR */
typedef struct virtio_video_queue_clear {
    struct virtio_video_cmd_hdr hdr;
    __le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
    __u8 padding[4];
} virtio_video_queue_clear;

/* VIRTIO_VIDEO_CMD_GET_PARAMS */
typedef struct virtio_video_plane_format {
    __le32 plane_size;
    __le32 stride;
} virtio_video_plane_format;

typedef struct virtio_video_crop {
    __le32 left;
    __le32 top;
    __le32 width;
    __le32 height;
} virtio_video_crop;

typedef struct virtio_video_params {
    __le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
    __le32 format; /* One of VIRTIO_VIDEO_FORMAT_* types */
    __le32 frame_width;
    __le32 frame_height;
    __le32 min_buffers;
    __le32 max_buffers;
    struct virtio_video_crop crop;
    __le32 frame_rate;
    __le32 num_planes;
    struct virtio_video_plane_format plane_formats[VIRTIO_VIDEO_MAX_PLANES];
} virtio_video_params;

typedef struct virtio_video_get_params {
    struct virtio_video_cmd_hdr hdr;
    __le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
    __u8 padding[4];
} virtio_video_get_params;

typedef struct virtio_video_get_params_resp {
    struct virtio_video_cmd_hdr hdr;
    struct virtio_video_params params;
} virtio_video_get_params_resp;

/* VIRTIO_VIDEO_CMD_SET_PARAMS */
typedef struct virtio_video_set_params {
    struct virtio_video_cmd_hdr hdr;
    struct virtio_video_params params;
} virtio_video_set_params;

/* VIRTIO_VIDEO_CMD_QUERY_CONTROL */
typedef enum virtio_video_control_type {
    VIRTIO_VIDEO_CONTROL_BITRATE = 1,
    VIRTIO_VIDEO_CONTROL_PROFILE,
    VIRTIO_VIDEO_CONTROL_LEVEL,
} virtio_video_control_type;

typedef struct virtio_video_query_control_profile {
    __le32 format; /* One of VIRTIO_VIDEO_FORMAT_* */
    __u8 padding[4];
} virtio_video_query_control_profile;

typedef struct virtio_video_query_control_level {
    __le32 format; /* One of VIRTIO_VIDEO_FORMAT_* */
    __u8 padding[4];
} virtio_video_query_control_level;

typedef struct virtio_video_query_control {
    struct virtio_video_cmd_hdr hdr;
    __le32 control; /* One of VIRTIO_VIDEO_CONTROL_* types */
    __u8 padding[4];
    /*
        * Followed by a value of struct virtio_video_query_control_*
        * in accordance with the value of control.
        */
} virtio_video_query_control;

typedef struct virtio_video_query_control_resp_profile {
    __le32 num;
    __u8 padding[4];
    /* Followed by an array le32 profiles[] */
} virtio_video_query_control_resp_profile;

typedef struct virtio_video_query_control_resp_level {
    __le32 num;
    __u8 padding[4];
    /* Followed by an array le32 level[] */
} virtio_video_query_control_resp_level;

typedef struct virtio_video_query_control_resp {
    struct virtio_video_cmd_hdr hdr;
    /* Followed by one of struct virtio_video_query_control_resp_* */
} virtio_video_query_control_resp;

/* VIRTIO_VIDEO_CMD_GET_CONTROL */
typedef struct virtio_video_get_control {
    struct virtio_video_cmd_hdr hdr;
    __le32 control; /* One of VIRTIO_VIDEO_CONTROL_* types */
    __u8 padding[4];
} virtio_video_get_control;

typedef struct virtio_video_control_val_bitrate {
    __le32 bitrate;
    __u8 padding[4];
} virtio_video_control_val_bitrate;

typedef struct virtio_video_control_val_profile {
    __le32 profile;
    __u8 padding[4];
} virtio_video_control_val_profile;

typedef struct virtio_video_control_val_level {
    __le32 level;
    __u8 padding[4];
} virtio_video_control_val_level;

typedef struct virtio_video_get_control_resp {
    struct virtio_video_cmd_hdr hdr;
    /* Followed by one of struct virtio_video_control_val_* */
} virtio_video_get_control_resp;

/* VIRTIO_VIDEO_CMD_SET_CONTROL */
typedef struct virtio_video_set_control {
    struct virtio_video_cmd_hdr hdr;
    __le32 control; /* One of VIRTIO_VIDEO_CONTROL_* types */
    __u8 padding[4];
    /* Followed by one of struct virtio_video_control_val_* */
} virtio_video_set_control;

typedef struct virtio_video_set_control_resp {
    struct virtio_video_cmd_hdr hdr;
} virtio_video_set_control_resp;

/*
 * Events
 */

typedef enum virtio_video_event_type {
    /* For all devices */
    VIRTIO_VIDEO_EVENT_ERROR = 0x0100,

    /* For decoder only */
    VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED = 0x0200,
} virtio_video_event_type;

typedef struct virtio_video_event {
    __le32 event_type; /* One of VIRTIO_VIDEO_EVENT_* types */
    __le32 stream_id;
} virtio_video_event;

/* VirtIO Video backend defines */

#define TYPE_VIRTIO_VIDEO_PCI_BASE "virtio-video-pci-base"
#define TYPE_VIRTIO_VIDEO_PCI "virtio-video-pci"
#define TYPE_VIRTIO_VIDEO_PCI_TRANS "virtio-video-pci-transitional"
#define TYPE_VIRTIO_VIDEO_PCI_NON_TRANS "virtio-video-pci-non-transitional"
#define TYPE_VIRTIO_VIDEO "virtio-video-device"

#define VIRTIO_VIDEO_DRM_DEVICE "/dev/dri/by-path/pci-0000:00:02.0-render"

#define VIRTIO_VIDEO_VM_VERSION 1
#define VIRTIO_VIDEO_VQ_SIZE 256

#define VIRTIO_VIDEO_VERSION 0
#define VIRTIO_VIDEO_CAPS_LENGTH_MAX 1024
#define VIRTIO_VIDEO_RESPONSE_LENGTH_MAX 1024

#define VIRTIO_VIDEO_PCI(obj) \
        OBJECT_CHECK(VirtIOVideoPCI, (obj), TYPE_VIRTIO_VIDEO_PCI_BASE)
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

typedef struct VirtIOVideoModel {
    virtio_video_device_model id;
    const char* name;
} VirtIOVideoModel;

typedef struct VirtIOVideoBackend {
    virtio_video_backend id;
    const char* name;
} VirtIOVideoBackend;

typedef struct VirtIOVideoControl {
    uint32_t value;
    QLIST_ENTRY(VirtIOVideoControl) next;
} VirtIOVideoControl;

typedef enum VirtIOVideoStreamEvent {
    VirtIOVideoStreamEventNone = 0,
    VirtIOVideoStreamEventParamChange,
    VirtIOVideoStreamEventStreamQueue,
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

typedef struct VirtIOVideoStreamResource {
    uint32_t resource_id;
    uint32_t planes_layout;
    uint32_t num_planes;
    __le32 plane_offsets[VIRTIO_VIDEO_MAX_PLANES];
    __le32 num_entries[VIRTIO_VIDEO_MAX_PLANES];
    void *mem[VIRTIO_VIDEO_MAX_PLANES];
    QLIST_ENTRY(VirtIOVideoStreamResource) next;
} VirtIOVideoStreamResource;

typedef struct VirtIOVideoStream {
    void *mfx_session;
    uint32_t mfxWaitMs;
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
    void *mfxSurfWork;
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

typedef struct VirtIOVideo {
    VirtIODevice parent_obj;
    struct {
        char *model;
        char *backend;
    } property;
    virtio_video_device_model model;
    virtio_video_backend backend;
    virtio_video_config config;
    VirtQueue *cmd_vq, *event_vq;
    QemuMutex ev_mutex;
    QLIST_HEAD(, VirtIOVideoStream) stream_list;
    int drm_fd;
    void *va_disp_handle;
    int32_t mfx_impl;
    uint16_t mfx_version_major;
    uint16_t mfx_version_minor;
    VirtIOVideoCaps caps_in;
    VirtIOVideoCaps caps_out;
} VirtIOVideo;

typedef struct VirtIOVideoPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOVideo vdev;
} VirtIOVideoPCI;

#endif /* QEMU_VIRTIO_VIDEO_H */
