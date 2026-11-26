/* SPDX-License-Identifier: MIT */

#include "no_signal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct rk_frame_spec {
    const char *name;
    int width;
    int height;
};

/* Sizes onekvm-no-signal-cache writes for the current language. */
static const struct rk_frame_spec rk_frames[] = {
    {"1920x1080", 1920, 1080},
    {"1280x720", 1280, 720},
    {"640x480", 640, 480},
};

static const char *rk_cache_root(void)
{
    const char *env = getenv("ONEKVM_NO_SIGNAL_CACHE");

    if (env != NULL && env[0] != '\0') {
        return env;
    }
    return "/data/onekvm/cache/no-signal";
}

static const struct rk_frame_spec *rk_nearest_frame(int width, int height)
{
    const struct rk_frame_spec *best = NULL;
    long long best_distance = -1;
    size_t i;

    for (i = 0; i < sizeof(rk_frames) / sizeof(rk_frames[0]); i++) {
        long long dw = (long long)rk_frames[i].width - width;
        long long dh = (long long)rk_frames[i].height - height;
        long long distance = dw * dw + dh * dh;

        if (best == NULL || distance < best_distance) {
            best = &rk_frames[i];
            best_distance = distance;
        }
    }
    return best;
}

static int rk_frame_path(const struct rk_frame_spec *frame, char *path, size_t path_size)
{
    int written = snprintf(path, path_size, "%s/current/%s.nv21", rk_cache_root(), frame->name);

    return written > 0 && (size_t)written < path_size ? 0 : -1;
}

void rk_no_signal_token(int width, int height, struct rk_no_signal_token *token)
{
    char path[512];
    struct stat info;

    if (token == NULL) {
        return;
    }
    memset(token, 0, sizeof(*token));
    if (rk_frame_path(rk_nearest_frame(width, height), path, sizeof(path)) != 0 ||
        stat(path, &info) != 0) {
        return;
    }
    token->device = (uint64_t)info.st_dev;
    token->inode = (uint64_t)info.st_ino;
    token->mtime_sec = (int64_t)info.st_mtim.tv_sec;
    token->mtime_nsec = (int64_t)info.st_mtim.tv_nsec;
}

static uint8_t *rk_load_nv21(const struct rk_frame_spec *frame)
{
    char path[512];
    FILE *file;
    size_t pixels = (size_t)frame->width * (size_t)frame->height;
    size_t expect = pixels + pixels / 2;
    uint8_t *nv21;
    unsigned char extra = 0;

    if (rk_frame_path(frame, path, sizeof(path)) != 0) {
        return NULL;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    nv21 = malloc(expect);
    if (nv21 != NULL &&
        (fread(nv21, 1, expect, file) != expect || fread(&extra, 1, 1, file) == 1)) {
        free(nv21);
        nv21 = NULL;
    }
    /* The frame is drawn once per change; its page cache (3 MiB at 1080p)
     * would otherwise stay charged to Core until memory pressure. */
    (void)posix_fadvise(fileno(file), 0, 0, POSIX_FADV_DONTNEED);
    fclose(file);
    return nv21;
}

int rk_no_signal_render_nv12(uint8_t *dst, size_t capacity, int width, int height,
                             uint32_t stride, uint32_t ver_stride)
{
    const struct rk_frame_spec *frame;
    uint8_t *nv21;
    uint8_t *chroma;
    const uint8_t *src_chroma;
    int fit_w;
    int fit_h;
    int x0;
    int y0;
    int x;
    int y;

    if (dst == NULL || width < 2 || height < 2 || (width & 1) || (height & 1) ||
        width > 4096 || height > 2160 || stride < (uint32_t)width ||
        ver_stride < (uint32_t)height ||
        capacity < (size_t)stride * ver_stride + (size_t)stride * (size_t)height / 2u) {
        return -1;
    }
    chroma = dst + (size_t)stride * ver_stride;
    /* Limited-range black, as the cache is BT.601 limited range. */
    memset(dst, 16, (size_t)stride * ver_stride);
    memset(chroma, 128, (size_t)stride * (size_t)height / 2u);
    frame = rk_nearest_frame(width, height);
    nv21 = rk_load_nv21(frame);
    if (nv21 == NULL) {
        return 0;
    }
    if ((long long)frame->width * height <= (long long)frame->height * width) {
        fit_h = height;
        fit_w = (int)((long long)frame->width * height / frame->height);
    } else {
        fit_w = width;
        fit_h = (int)((long long)frame->height * width / frame->width);
    }
    fit_w &= ~1;
    fit_h &= ~1;
    if (fit_w < 2 || fit_h < 2) {
        free(nv21);
        return 0;
    }
    x0 = ((width - fit_w) / 2) & ~1;
    y0 = ((height - fit_h) / 2) & ~1;
    for (y = 0; y < fit_h; y++) {
        const uint8_t *src = nv21 + (size_t)(y * frame->height / fit_h) * (size_t)frame->width;
        uint8_t *row = dst + (size_t)(y0 + y) * stride + (size_t)x0;

        for (x = 0; x < fit_w; x++) {
            row[x] = src[x * frame->width / fit_w];
        }
    }
    /* NV21 stores V then U; NV12 wants U then V. */
    src_chroma = nv21 + (size_t)frame->width * (size_t)frame->height;
    for (y = 0; y < fit_h / 2; y++) {
        const uint8_t *src =
            src_chroma + (size_t)(y * (frame->height / 2) / (fit_h / 2)) * (size_t)frame->width;
        uint8_t *row = chroma + (size_t)(y0 / 2 + y) * stride + (size_t)x0;

        for (x = 0; x < fit_w / 2; x++) {
            int sx = x * (frame->width / 2) / (fit_w / 2);

            row[x * 2] = src[sx * 2 + 1];
            row[x * 2 + 1] = src[sx * 2];
        }
    }
    free(nv21);
    return 0;
}
