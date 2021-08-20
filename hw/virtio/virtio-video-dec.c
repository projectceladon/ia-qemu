/*
 * VirtIO-Video Backend Driver
 * VirtIO-Video Backend Decoder
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
#include "qemu/osdep.h"
#include "virtio-video-dec.h"
#include "virtio-video-msdk.h"
#include "virtio-video-vaapi.h"

#define VIRTIO_VIDEO_DECODE_THREAD "Virtio-Video-Decode"

size_t virtio_video_dec_cmd_query_capability(VirtIODevice *vdev,
    virtio_video_query_capability *req, virtio_video_query_capability_resp **resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    size_t len = 0;
    void *src;
    VIRTVID_DEBUG("    %s: stream 0x%x, queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->queue_type);

    if (req != NULL && *resp == NULL) {
        switch (req->queue_type) {
        case VIRTIO_VIDEO_QUEUE_TYPE_INPUT:
            len = vid->caps_in.size;
            src = vid->caps_in.ptr;
            break;
        case VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT:
            len = vid->caps_out.size;
            src = vid->caps_out.ptr;
            break;
        default:
            break;
        }

        *resp = g_malloc0(len);
        if (*resp != NULL) {
            memcpy(*resp, src, len);
            (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY;
            (*resp)->hdr.stream_id = req->hdr.stream_id;
        } else {
            len = 0;
        }
    }

    return len;
}

static void *virtio_video_decode_thread(void *arg)
{
    VirtIOVideoStream *stream = arg;
    sigset_t sigmask, old;
    int err;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGINT);
    err = pthread_sigmask(SIG_BLOCK, &sigmask, &old);
    if (err) {
        VIRTVID_ERROR("%s thread %d change SIG_BLOCK failed err %d\n", VIRTIO_VIDEO_DECODE_THREAD, stream->stream_id, err);
    }

    VIRTVID_DEBUG("%s thread %d running\n", VIRTIO_VIDEO_DECODE_THREAD, stream->stream_id);
    // while (1) {
    // }

    err = pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (err) {
        VIRTVID_ERROR("%s thread %d restore old sigmask failed err %d\n", VIRTIO_VIDEO_DECODE_THREAD, stream->stream_id, err);
    }

    VIRTVID_DEBUG("%s thread %d exits\n", VIRTIO_VIDEO_DECODE_THREAD, stream->stream_id);

    return NULL;
}

size_t virtio_video_dec_cmd_stream_create(VirtIODevice *vdev,
    virtio_video_stream_create *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);
    if (virtio_video_msdk_find_format(&(vid->caps_in), req->coded_format, NULL)) {
        mfxStatus sts = MFX_ERR_NONE;
        VirtIOVideoStream *node = NULL;
        mfxInitParam par = {
            .Implementation = vid->mfx_impl,
            .Version.Major = vid->mfx_version_major,
            .Version.Minor = vid->mfx_version_minor,
        };

        node = g_malloc0(sizeof(VirtIOVideoStream));
        if (node) {
            VirtIOVideoControl *control = NULL;
            virtio_video_format_desc *desc = NULL;
            mfxVideoParam inParam = {0}, outParam = {0};
            int min, max;
            node->stream_id = ~0L;

            sts = MFXInitEx(par, (mfxSession*)&node->mfx_session);
            if (sts != MFX_ERR_NONE) {
                VIRTVID_ERROR("    %s: MFXInitEx returns %d for stream 0x%x", __FUNCTION__, sts, req->hdr.stream_id);
                g_free(node);
                goto OUT;
            }

            sts = MFXVideoCORE_SetHandle(node->mfx_session, MFX_HANDLE_VA_DISPLAY, (mfxHDL)vid->va_disp_handle);
            if (sts != MFX_ERR_NONE) {
                VIRTVID_ERROR("    %s: MFXVideoCORE_SetHandle returns %d for stream 0x%x", __FUNCTION__, sts, req->hdr.stream_id);
                MFXClose(node->mfx_session);
                g_free(node);
                goto OUT;
            }

            virtio_video_msdk_load_plugin(vdev, node->mfx_session, req->coded_format, false, false);

            node->stream_id = req->hdr.stream_id;
            node->in_mem_type = req->in_mem_type;
            node->out_mem_type = req->out_mem_type;
            node->in_format = req->coded_format;
            memcpy(node->tag, req->tag, strlen((char*)req->tag));

            // Try query all profiles
            QLIST_INIT(&node->control_caps.profile.list);
            virtio_video_msdk_fill_video_params(req->coded_format, &inParam);
            memset(&outParam, 0, sizeof(outParam));
            outParam.mfx.CodecId = inParam.mfx.CodecId;
            virtio_video_profile_range(req->coded_format, &min, &max);
            if (min != max) {
                int profile;
                for (profile = min; profile <= max; profile++) {
                    inParam.mfx.CodecProfile = virtio_video_profile_to_mfx(req->coded_format, profile);
                    if (inParam.mfx.CodecProfile != MFX_PROFILE_UNKNOWN) {
                        sts = MFXVideoDECODE_Query(node->mfx_session, &inParam, &outParam);
                        if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                            node->control.profile = profile;
                            control = g_malloc0(sizeof(VirtIOVideoControl));
                            if (control) {
                                node->control_caps.profile.num++;
                                control->value = profile;
                                QLIST_INSERT_HEAD(&node->control_caps.profile.list, control, next);
                            }
                        }
                    }
                }
            }

            // Try query all levels
            QLIST_INIT(&node->control_caps.level.list);
            virtio_video_msdk_fill_video_params(req->coded_format, &inParam);
            memset(&outParam, 0, sizeof(outParam));
            outParam.mfx.CodecId = inParam.mfx.CodecId;
            virtio_video_level_range(req->coded_format, &min, &max);
            if (min != max) {
                int level;
                for (level = min; level <= max; level++) {
                    inParam.mfx.CodecLevel = virtio_video_level_to_mfx(req->coded_format, level);
                    if (inParam.mfx.CodecLevel != MFX_LEVEL_UNKNOWN) {
                        sts = MFXVideoDECODE_Query(node->mfx_session, &inParam, &outParam);
                        if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                            node->control.level = level;
                            control = g_malloc0(sizeof(VirtIOVideoControl));
                            if (control) {
                                node->control_caps.level.num++;
                                control->value = level;
                                QLIST_INSERT_HEAD(&node->control_caps.level.list, control, next);
                            }
                        }
                    }
                }
            }

            virtio_video_msdk_fill_video_params(req->coded_format, &inParam);
            memset(&outParam, 0, sizeof(outParam));
            outParam.mfx.CodecId = inParam.mfx.CodecId;
            inParam.mfx.TargetKbps = 10000; //TODO: Determine the max bitrage
            sts = MFXVideoDECODE_Query(node->mfx_session, &inParam, &outParam);
            if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                node->control.bitrate = inParam.mfx.TargetKbps;
            }

            memset(&node->in_params, 0, sizeof(node->in_params));

            if (virtio_video_msdk_find_format(&(vid->caps_in), node->in_format, &desc)) {
                node->in_params.frame_width = ((virtio_video_format_frame*)((void*)desc + sizeof(virtio_video_format_desc)))->width.max;
                node->in_params.frame_height = ((virtio_video_format_frame*)((void*)desc + sizeof(virtio_video_format_desc)))->height.max;
                node->in_params.min_buffers = 1;
                node->in_params.max_buffers = 1;
                node->in_params.crop.left = 0;
                node->in_params.crop.top = 0;
                node->in_params.crop.width = node->in_params.frame_width;
                node->in_params.crop.height = node->in_params.frame_height;
                node->in_params.frame_rate = ((virtio_video_format_range*)((void*)desc + sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame)))->max;

                memcpy(&node->out_params, &node->in_params, sizeof(node->in_params));

                // For VIRTIO_VIDEO_QUEUE_TYPE_INPUT
                node->in_params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
                node->in_params.format = node->in_format;
                // TODO: what's the definition of plane number, size and stride for coded format?
                node->in_params.num_planes = 1;
                node->in_params.plane_formats[0].plane_size = 0;
                node->in_params.plane_formats[0].stride = 0;

                // For VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT
                node->out_params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
                node->out_params.format = VIRTIO_VIDEO_FORMAT_NV12;
                node->out_params.num_planes = 2;
                node->out_params.plane_formats[0].plane_size = node->out_params.frame_width * node->out_params.frame_height;
                node->out_params.plane_formats[0].stride = node->out_params.frame_width;
                node->out_params.plane_formats[1].plane_size = node->out_params.frame_width * node->out_params.frame_height / 2;
                node->out_params.plane_formats[1].stride = node->out_params.frame_width;
            }

            qemu_mutex_init(&node->mutex);
            node->event_vq = vid->event_vq;

            qemu_thread_create(&node->thread, VIRTIO_VIDEO_DECODE_THREAD, virtio_video_decode_thread, node, QEMU_THREAD_JOINABLE);

            QLIST_INSERT_HEAD(&vid->stream_list, node, next);

            VIRTVID_DEBUG("    %s: stream 0x%x created", __FUNCTION__, req->hdr.stream_id);
            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
        }
    } else {
        VIRTVID_ERROR("    %s: stream 0x%x, unsupported format 0x%x", __FUNCTION__, req->hdr.stream_id, req->coded_format);
    }

OUT:
    return len;
}

size_t virtio_video_dec_cmd_stream_destroy(VirtIODevice *vdev,
    virtio_video_stream_destroy *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    VirtIOVideoControl *control, *next_ctrl = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            // TODO: close decoder
            virtio_video_msdk_load_plugin(vdev, node->mfx_session, node->in_format, false, true);
            MFXClose(node->mfx_session);
            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            QLIST_FOREACH_SAFE(control, &node->control_caps.profile.list, next, next_ctrl) {
                QLIST_REMOVE(control, next);
                g_free(control);
            }
            QLIST_FOREACH_SAFE(control, &node->control_caps.level.list, next, next_ctrl) {
                QLIST_REMOVE(control, next);
                g_free(control);
            }
            qemu_mutex_destroy(&node->mutex);

            // TODO: Notify thread to stop, or send SIGTERM
            pthread_kill(node->thread.thread, SIGTERM);
            qemu_thread_join(&node->thread);
            node->thread.thread = 0;

            QLIST_REMOVE(node, next);
            g_free(node);
            VIRTVID_DEBUG("    %s: stream 0x%x destroyed", __FUNCTION__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_resource_destroy_all(VirtIODevice *vdev,
    virtio_video_resource_destroy_all *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            // TODO: Drain codec
            // TODO: Destroy all resources
            if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {

            } else {

            }

            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            VIRTVID_DEBUG("    %s: stream 0x%x queue_type 0x%x all resource destroyed", __FUNCTION__, req->hdr.stream_id, req->queue_type);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_get_params(VirtIODevice *vdev,
    virtio_video_get_params *req, virtio_video_get_params_resp *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_PARAMS;
            if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                memcpy(&resp->params, &node->in_params, sizeof(resp->params));
            } else if (req->queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                memcpy(&resp->params, &node->out_params, sizeof(resp->params));
            } else {
                resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->queue_type);
            }
            VIRTVID_DEBUG("    %s: stream 0x%x", __FUNCTION__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_set_params(VirtIODevice *vdev,
    virtio_video_set_params *req, virtio_video_cmd_hdr *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            resp->type = VIRTIO_VIDEO_RESP_OK_NODATA;
            if (req->params.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                memcpy(&node->in_params, &req->params, sizeof(req->params));
            } else if (req->params.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                memcpy(&node->out_params, &req->params, sizeof(req->params));
            } else {
                resp->type = VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION;
                VIRTVID_ERROR("    %s: stream 0x%x, unsupported queue_type 0x%x", __FUNCTION__, req->hdr.stream_id, req->params.queue_type);
            }
            VIRTVID_DEBUG("    %s: stream 0x%x", __FUNCTION__, req->hdr.stream_id);
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_query_control(VirtIODevice *vdev,
    virtio_video_query_control *req, virtio_video_query_control_resp **resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    *resp = NULL;
    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            if (req->control == VIRTIO_VIDEO_CONTROL_PROFILE) {
                virtio_video_format format = ((virtio_video_query_control_profile*)((void*)req + sizeof(virtio_video_query_control)))->format;

                if (format == node->in_format) {
                    len = sizeof(virtio_video_query_control_resp);
                    len += sizeof(virtio_video_query_control_resp_profile);
                    len += node->control_caps.profile.num * sizeof(__le32);
                } else {
                    VIRTVID_ERROR("    %s: stream 0x%x format %d mismatch requested %d", __FUNCTION__, req->hdr.stream_id, node->in_format, format);
                }

                *resp = g_malloc0(len);
                if (*resp != NULL) {
                    VirtIOVideoControl *control, *next_ctrl = NULL;
                    __le32 *profile = (void*)(*resp) + sizeof(virtio_video_query_control_resp) + sizeof(virtio_video_query_control_resp_profile);

                    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL;
                    QLIST_FOREACH_SAFE(control, &node->control_caps.profile.list, next, next_ctrl) {
                        *profile = control->value;
                        ((virtio_video_query_control_resp_profile*)((void*)(*resp) + sizeof(virtio_video_query_control_resp)))->num++;
                        profile++;
                    }
                } else {
                    len = 0;
                }
                VIRTVID_DEBUG("    %s: stream 0x%x support %d profiles", __FUNCTION__, req->hdr.stream_id, node->control_caps.profile.num);
            } else if (req->control == VIRTIO_VIDEO_CONTROL_LEVEL) {
                virtio_video_format format = ((virtio_video_query_control_level*)((void*)req + sizeof(virtio_video_query_control)))->format;

                if (format == node->in_format) {
                    len = sizeof(virtio_video_query_control_resp);
                    len += sizeof(virtio_video_query_control_resp_level);
                    len += node->control_caps.level.num * sizeof(__le32);
                } else {
                    VIRTVID_ERROR("    %s: stream 0x%x format %d mismatch requested %d", __FUNCTION__, req->hdr.stream_id, node->in_format, format);
                }

                *resp = g_malloc0(len);
                if (*resp != NULL) {
                    VirtIOVideoControl *control, *next_ctrl = NULL;
                    __le32 *level = (void*)(*resp) + sizeof(virtio_video_query_control_resp) + sizeof(virtio_video_query_control_resp_level);

                    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL;
                    QLIST_FOREACH_SAFE(control, &node->control_caps.level.list, next, next_ctrl) {
                        *level = control->value;
                        ((virtio_video_query_control_resp_level*)((void*)(*resp) + sizeof(virtio_video_query_control_resp)))->num++;
                        level++;
                    }
                } else {
                    len = 0;
                }
                VIRTVID_DEBUG("    %s: stream 0x%x support %d levels", __FUNCTION__, req->hdr.stream_id, node->control_caps.level.num);
            } else {
                len = sizeof(virtio_video_query_control_resp);
                *resp = g_malloc0(len);
                if (*resp != NULL) {
                    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
                    (*resp)->hdr.stream_id = req->hdr.stream_id;
                } else {
                    len = 0;
                }
                VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __FUNCTION__, req->hdr.stream_id, req->control);
            }
            break;
        }
    }

    if (*resp == NULL) {
        len = sizeof(virtio_video_query_control_resp);
        *resp = g_malloc0(len);
        if (*resp != NULL) {
            (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
            (*resp)->hdr.stream_id = req->hdr.stream_id;
        } else {
            len = 0;
        }
    }

    return len;
}

size_t virtio_video_dec_cmd_get_control(VirtIODevice *vdev,
    virtio_video_get_control *req, virtio_video_get_control_resp **resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    switch (req->control) {
    case VIRTIO_VIDEO_CONTROL_BITRATE:
        len = sizeof(virtio_video_get_control_resp) + sizeof(virtio_video_control_val_bitrate);
        break;
    case VIRTIO_VIDEO_CONTROL_PROFILE:
        len = sizeof(virtio_video_get_control_resp) + sizeof(virtio_video_control_val_profile);
        break;
    case VIRTIO_VIDEO_CONTROL_LEVEL:
        len = sizeof(virtio_video_get_control_resp) + sizeof(virtio_video_control_val_level);
        break;
    default:
        len = sizeof(virtio_video_get_control_resp);
        VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __FUNCTION__, req->hdr.stream_id, req->control);
        break;
    }

    *resp = g_malloc0(len);
    if (*resp != NULL) {
        (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
        (*resp)->hdr.stream_id = req->hdr.stream_id;
        QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
            if (node->stream_id == req->hdr.stream_id) {
                (*resp)->hdr.type = VIRTIO_VIDEO_RESP_OK_GET_CONTROL;
                if (req->control == VIRTIO_VIDEO_CONTROL_BITRATE) {
                    ((virtio_video_control_val_bitrate*)((void*)(*resp) + sizeof(virtio_video_get_control_resp)))->bitrate = node->control.bitrate;
                    VIRTVID_DEBUG("    %s: stream 0x%x bitrate %d", __FUNCTION__, req->hdr.stream_id, node->control.bitrate);
                } else if (req->control == VIRTIO_VIDEO_CONTROL_PROFILE) {
                    ((virtio_video_control_val_profile*)((void*)(*resp) + sizeof(virtio_video_get_control_resp)))->profile = node->control.profile;
                    VIRTVID_DEBUG("    %s: stream 0x%x profile %d", __FUNCTION__, req->hdr.stream_id, node->control.profile);
                } else if (req->control == VIRTIO_VIDEO_CONTROL_LEVEL) {
                    ((virtio_video_control_val_level*)((void*)(*resp) + sizeof(virtio_video_get_control_resp)))->level = node->control.level;
                    VIRTVID_DEBUG("    %s: stream 0x%x level %d", __FUNCTION__, req->hdr.stream_id, node->control.level);
                } else {
                    (*resp)->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
                    VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __FUNCTION__, req->hdr.stream_id, req->control);
                }
                break;
            }
        }
    } else {
        len = 0;
    }

    return len;
}

size_t virtio_video_dec_cmd_set_control(VirtIODevice *vdev,
    virtio_video_set_control *req, virtio_video_set_control_resp *resp)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    size_t len = 0;

    resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID;
    resp->hdr.stream_id = req->hdr.stream_id;
    len = sizeof(*resp);

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        if (node->stream_id == req->hdr.stream_id) {
            resp->hdr.type = VIRTIO_VIDEO_RESP_OK_NODATA;
            if (req->control == VIRTIO_VIDEO_CONTROL_BITRATE) {
                node->control.bitrate = ((virtio_video_control_val_bitrate*)(((void*)req) + sizeof(virtio_video_set_control)))->bitrate;
                VIRTVID_DEBUG("    %s: stream 0x%x bitrate %d", __FUNCTION__, req->hdr.stream_id, node->control.bitrate);
            } else if (req->control == VIRTIO_VIDEO_CONTROL_PROFILE) {
                node->control.profile = ((virtio_video_control_val_profile*)(((void*)req) + sizeof(virtio_video_set_control)))->profile;
                VIRTVID_DEBUG("    %s: stream 0x%x profile %d", __FUNCTION__, req->hdr.stream_id, node->control.profile);
            } else if (req->control == VIRTIO_VIDEO_CONTROL_LEVEL) {
                node->control.level = ((virtio_video_control_val_level*)(((void*)req) + sizeof(virtio_video_set_control)))->level;
                VIRTVID_DEBUG("    %s: stream 0x%x level %d", __FUNCTION__, req->hdr.stream_id, node->control.level);
            } else {
                resp->hdr.type = VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL;
                VIRTVID_ERROR("    %s: stream 0x%x unsupported control %d", __FUNCTION__, req->hdr.stream_id, req->control);
            }
            break;
        }
    }

    return len;
}

size_t virtio_video_dec_event(VirtIODevice *vdev, virtio_video_event *ev)
{
    //VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    size_t len = 0;

    if (ev) {
        //ev->stream_id = ++vid->stream_id;
        len = sizeof(*ev);
        VIRTVID_DEBUG("    %s: event_type 0x%x, stream_id 0x%x", __FUNCTION__, ev->event_type, ev->stream_id);
    } else {
        VIRTVID_ERROR("Invalid virtio_video_event buffer");
    }

    return len;
}

static int virtio_video_decode_init_msdk(VirtIODevice *vdev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    mfxStatus sts = MFX_ERR_NONE;
    virtio_video_format coded_format;
    mfxSession mfx_session;

    vid->mfx_impl = MFX_IMPL_AUTO_ANY;//MFX_IMPL_HARDWARE
    vid->mfx_version_major = 1;
    vid->mfx_version_minor = 0;

    mfxInitParam par = {
        .Implementation = vid->mfx_impl,
        .Version.Major = vid->mfx_version_major,
        .Version.Minor = vid->mfx_version_minor,
    };

    mfxVideoParam inParam = {0}, outParam = {0};

    if (virtio_video_create_va_env_drm(vdev)) {
        VIRTVID_ERROR("Fail to create VA environment on DRM");
        return -1;
    }

    sts = MFXInitEx(par, &mfx_session);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("MFXInitEx returns %d", sts);
        return -1;
    }

    sts = MFXVideoCORE_SetHandle(mfx_session, MFX_HANDLE_VA_DISPLAY, (mfxHDL)vid->va_disp_handle);
    if (sts != MFX_ERR_NONE) {
        VIRTVID_ERROR("MFXVideoCORE_SetHandle returns %d", sts);
        MFXClose(mfx_session);
        return -1;
    }

    for (coded_format = VIRTIO_VIDEO_FORMAT_CODED_MIN;
         coded_format <= VIRTIO_VIDEO_FORMAT_CODED_MAX;
         coded_format++) {
        int coded_mfx4cc = virito_video_format_to_mfx4cc(coded_format);
        if (coded_mfx4cc != 0) {
            // Query CodecId to fill virtio_video_format_desc
            memset(&outParam, 0, sizeof(outParam));
            outParam.mfx.CodecId = coded_mfx4cc;
            sts = MFXVideoDECODE_Query(mfx_session, NULL, &outParam);
            if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                void *buf = NULL;
                uint32_t w_min = 0, h_min = 0, w_max = 0, h_max = 0;

                // Add a new virtio_video_format_desc block since current format is supported
                ++((virtio_video_query_capability_resp*)vid->caps_in.ptr)->num_descs;

                // Save old caps, allocate a larger buffer, copy it back
                buf = g_malloc0(vid->caps_in.size);
                memcpy(buf, vid->caps_in.ptr, vid->caps_in.size);
                g_free(vid->caps_in.ptr);
                vid->caps_in.size += sizeof(virtio_video_format_desc);
                vid->caps_in.ptr = g_malloc0(vid->caps_in.size);
                memcpy(vid->caps_in.ptr, buf, vid->caps_in.size - sizeof(virtio_video_format_desc));
                g_free(buf);

                // Append the newly added virtio_video_format_desc
                buf = (char*)vid->caps_in.ptr + vid->caps_in.size - sizeof(virtio_video_format_desc);
                virtio_video_msdk_fill_format_desc(coded_format, (virtio_video_format_desc*)buf);

                // Try query max & min size for a coded format
                virtio_video_msdk_fill_video_params(coded_format, &inParam);
                memset(&outParam, 0, sizeof(outParam));
                outParam.mfx.CodecId = inParam.mfx.CodecId;

                inParam.mfx.FrameInfo.Width = VIRTIO_VIDEO_MSDK_DIMENSION_MAX;
                inParam.mfx.FrameInfo.Height = VIRTIO_VIDEO_MSDK_DIMENSION_MAX;

                virtio_video_msdk_load_plugin(vdev, mfx_session, coded_format, false, false);
                do {
                    sts = MFXVideoDECODE_Query(mfx_session, &inParam, &outParam);
                    if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                        w_max = outParam.mfx.FrameInfo.Width;
                        h_max = outParam.mfx.FrameInfo.Height;
                        break;
                    }
                    inParam.mfx.FrameInfo.Width -= VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
                    if (inParam.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) {
                        inParam.mfx.FrameInfo.Height -= VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
                    } else {
                        inParam.mfx.FrameInfo.Height -= VIRTIO_VIDEO_MSDK_DIM_STEP_OTHER;
                    }
                } while (inParam.mfx.FrameInfo.Width >= VIRTIO_VIDEO_MSDK_DIMENSION_MIN && inParam.mfx.FrameInfo.Height >= VIRTIO_VIDEO_MSDK_DIMENSION_MIN);

                inParam.mfx.FrameInfo.Width = VIRTIO_VIDEO_MSDK_DIMENSION_MIN;
                inParam.mfx.FrameInfo.Height = VIRTIO_VIDEO_MSDK_DIMENSION_MIN;
                do {
                    sts = MFXVideoDECODE_Query(mfx_session, &inParam, &outParam);
                    if (sts == MFX_ERR_NONE || sts == MFX_WRN_PARTIAL_ACCELERATION) {
                        w_min = outParam.mfx.FrameInfo.Width;
                        h_min = outParam.mfx.FrameInfo.Height;
                        break;
                    }
                    inParam.mfx.FrameInfo.Width += VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
                    if (inParam.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) {
                        inParam.mfx.FrameInfo.Height += VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
                    } else {
                        inParam.mfx.FrameInfo.Height += VIRTIO_VIDEO_MSDK_DIM_STEP_OTHER;
                    }
                } while (inParam.mfx.FrameInfo.Width <= w_max && inParam.mfx.FrameInfo.Height <= h_max);
                virtio_video_msdk_load_plugin(vdev, mfx_session, coded_format, false, true);

                // Add one virtio_video_format_frame and virtio_video_format_range block to last added virtio_video_format_desc
                if (w_min && w_max && h_min && h_max) {
                    void *buf_out = NULL;
                    uint32_t pos = 0;
                    uint32_t desc_size = 0;

                    buf = (char*)vid->caps_in.ptr + vid->caps_in.size - sizeof(virtio_video_format_desc);
                    ((virtio_video_format_desc*)buf)->num_frames = 1;

                    // Save old caps, allocate a larger buffer, copy it back
                    buf = g_malloc0(vid->caps_in.size);
                    memcpy(buf, vid->caps_in.ptr, vid->caps_in.size);
                    g_free(vid->caps_in.ptr);
                    vid->caps_in.size += sizeof(virtio_video_format_frame);
                    vid->caps_in.size += sizeof(virtio_video_format_range);
                    vid->caps_in.ptr = g_malloc0(vid->caps_in.size);
                    memcpy(vid->caps_in.ptr, buf,
                           vid->caps_in.size - sizeof(virtio_video_format_frame) - sizeof(virtio_video_format_range));
                    g_free(buf);

                    // Append the newly added virtio_video_format_frame and virtio_video_format_range
                    buf = (char*)vid->caps_in.ptr + vid->caps_in.size;
                    buf -= sizeof(virtio_video_format_frame);
                    buf -= sizeof(virtio_video_format_range);
                    ((virtio_video_format_frame*)buf)->width.min = w_min;
                    ((virtio_video_format_frame*)buf)->width.max = w_max;
                    ((virtio_video_format_frame*)buf)->width.step = VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE;
                    ((virtio_video_format_frame*)buf)->height.min = h_min;
                    ((virtio_video_format_frame*)buf)->height.max = h_max;
                    ((virtio_video_format_frame*)buf)->height.step =
                        (inParam.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ?
                        VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHER;
                    ((virtio_video_format_frame*)buf)->num_rates = 1;

                    buf += sizeof(virtio_video_format_frame);
                    // For decoding, frame rate may be unspecified, so always set range [1,60]
                    ((virtio_video_format_range*)buf)->min = 1;
                    ((virtio_video_format_range*)buf)->max = 60;
                    ((virtio_video_format_range*)buf)->step = 1;

                    VIRTVID_DEBUG("Add input caps for format %x, width [%d, %d]@%d, height [%d, %d]@%d, rate [%d, %d]@%d",
                                  coded_format,
                                  w_min, w_max, VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE,
                                  h_min, h_max, (inParam.mfx.FrameInfo.PicStruct == MFX_PICSTRUCT_PROGRESSIVE) ? VIRTIO_VIDEO_MSDK_DIM_STEP_PROGRESSIVE : VIRTIO_VIDEO_MSDK_DIM_STEP_OTHER,
                                  ((virtio_video_format_range*)buf)->min,
                                  ((virtio_video_format_range*)buf)->max,
                                  ((virtio_video_format_range*)buf)->step);

                    // Allocate a new block for output cap, copy format_frame format_desc from latest input cap
                    desc_size = sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame) + sizeof(virtio_video_format_range);
                    buf_out = g_malloc0(desc_size);
                    virtio_video_msdk_fill_format_desc(VIRTIO_VIDEO_FORMAT_NV12, (virtio_video_format_desc*)buf_out);
                    ((virtio_video_format_desc*)buf_out)->num_frames = 1;
                    buf -= sizeof(virtio_video_format_frame);
                    memcpy(buf_out + sizeof(virtio_video_format_desc), buf, sizeof(virtio_video_format_frame) + sizeof(virtio_video_format_range));

                    // Check if caps_out already have the format, add if not exist
                    if (!virtio_video_msdk_find_format_desc(&(vid->caps_out), (virtio_video_format_desc*)buf_out)) {
                        pos = vid->caps_out.size;

                        ++((virtio_video_query_capability_resp*)vid->caps_out.ptr)->num_descs;
                        // Save old caps, allocate a larger buffer, copy it back
                        buf = g_malloc0(vid->caps_out.size);
                        memcpy(buf, vid->caps_out.ptr, vid->caps_out.size);
                        g_free(vid->caps_out.ptr);
                        vid->caps_out.size += desc_size;
                        vid->caps_out.ptr = g_malloc0(vid->caps_out.size);
                        memcpy(vid->caps_out.ptr, buf, pos);
                        g_free(buf);

                        // Append the newly added virtio_video_format_desc, virtio_video_format_frame and virtio_video_format_range
                        memcpy(vid->caps_out.ptr + pos, buf_out, desc_size);

                        VIRTVID_DEBUG("Add output caps for format %x, width [%d, %d]@%d, height [%d, %d]@%d, rate [%d, %d]@%d",
                                      VIRTIO_VIDEO_FORMAT_NV12,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->width.min,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->width.max,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->width.step,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->height.min,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->height.max,
                                      ((virtio_video_format_frame*)(buf_out + sizeof(virtio_video_format_desc)))->height.step,
                                      ((virtio_video_format_range*)(buf_out + sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame)))->min,
                                      ((virtio_video_format_range*)(buf_out + sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame)))->max,
                                      ((virtio_video_format_range*)(buf_out + sizeof(virtio_video_format_desc) + sizeof(virtio_video_format_frame)))->step
                                      );
                    }
                    g_free(buf_out);
                }
            } else {
                VIRTVID_DEBUG("format %x isn't supported by MSDK, status %d", coded_format, sts);
            }
        }
    }

    MFXClose(mfx_session);

    return 0;
}

static void virtio_video_decode_destroy_msdk(VirtIODevice *vdev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    VirtIOVideoStream *node, *next = NULL;
    VirtIOVideoControl *control, *next_ctrl = NULL;

    QLIST_FOREACH_SAFE(node, &vid->stream_list, next, next) {
        MFXClose(node->mfx_session);
        QLIST_FOREACH_SAFE(control, &node->control_caps.profile.list, next, next_ctrl) {
            QLIST_REMOVE(control, next);
            g_free(control);
        }
        QLIST_FOREACH_SAFE(control, &node->control_caps.level.list, next, next_ctrl) {
            QLIST_REMOVE(control, next);
            g_free(control);
        }
        QLIST_REMOVE(node, next);
        g_free(node);
    }

    virtio_video_destroy_va_env_drm(vdev);
}

int virtio_video_decode_init(VirtIODevice *vdev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);
    int ret = -1;

    switch (vid->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        ret = virtio_video_decode_init_msdk(vdev);
    default:
        break;
    }

    VIRTVID_DEBUG("Decoder %s:%s initialized %d", vid->property.model, vid->property.backend, ret);

    return ret;
}

void virtio_video_decode_destroy(VirtIODevice *vdev)
{
    VirtIOVideo *vid = VIRTIO_VIDEO(vdev);

    switch (vid->backend) {
    case VIRTIO_VIDEO_BACKEND_MEDIA_SDK:
        virtio_video_decode_destroy_msdk(vdev);
    default:
        break;
    }
}