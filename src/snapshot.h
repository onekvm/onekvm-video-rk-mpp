#ifndef ONEKVM_VIDEO_RK_MPP_SNAPSHOT_H
#define ONEKVM_VIDEO_RK_MPP_SNAPSHOT_H

#include <onekvm/video_backend_v1.h>
#include <stdbool.h>
#include <stddef.h>

int32_t rk_source_snapshot(void *source,
    const struct onekvm_video_snapshot_request_v1 *request,
    uint8_t *data, uint64_t capacity, struct onekvm_video_snapshot_result_v1 *result,
    char *error, uint32_t error_capacity);
void rk_snapshot_offer(void *source, int fd, int width, int height, uint32_t fourcc,
                        uint32_t stride, size_t bytes, uint64_t pts_ns);
bool rk_snapshot_holds(void *source, int fd);
bool rk_snapshot_source_busy(void *source);
void rk_snapshot_quiesce(void *source);

#endif
