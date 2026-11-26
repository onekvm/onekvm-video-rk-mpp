/* SPDX-License-Identifier: MIT */

#include "no_signal.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[PATH_MAX];

static void path_of(char *path, size_t size, const char *name)
{
    int written = snprintf(path, size, "%s/%s", root, name);

    assert(written > 0 && (size_t)written < size);
}

/* An NV21 frame of one colour: Y, then V and U interleaved. */
static void write_nv21(const char *name, int width, int height, uint8_t y, uint8_t v, uint8_t u,
                       size_t trim)
{
    char path[PATH_MAX];
    size_t pixels = (size_t)width * (size_t)height;
    size_t size = pixels + pixels / 2 - trim;
    uint8_t *data = malloc(pixels + pixels / 2);
    FILE *file;
    size_t i;

    assert(data != NULL);
    memset(data, y, pixels);
    for (i = pixels; i < pixels + pixels / 2; i += 2) {
        data[i] = v;
        data[i + 1] = u;
    }
    path_of(path, sizeof(path), name);
    file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
    free(data);
}

static void point_current(const char *target)
{
    char path[PATH_MAX];

    path_of(path, sizeof(path), "current");
    (void)unlink(path);
    assert(symlink(target, path) == 0);
}

static uint8_t *frame(uint32_t stride, uint32_t ver_stride, int height, size_t *capacity)
{
    *capacity = (size_t)stride * ver_stride + (size_t)stride * (size_t)height / 2u;
    return calloc(1, *capacity);
}

static void expect_luma(const uint8_t *dst, uint32_t stride, int x, int y, uint8_t value)
{
    assert(dst[(size_t)y * stride + (size_t)x] == value);
}

/* x is the even luma column of the chroma pair. */
static void expect_chroma(const uint8_t *dst, uint32_t stride, uint32_t ver_stride, int x, int y,
                          uint8_t u, uint8_t v)
{
    const uint8_t *row = dst + (size_t)stride * ver_stride + (size_t)(y / 2) * stride;

    assert(row[x & ~1] == u && row[(x & ~1) + 1] == v);
}

int main(void)
{
    struct rk_no_signal_token none;
    struct rk_no_signal_token zh;
    struct rk_no_signal_token en;
    struct rk_no_signal_token zero;
    char path[PATH_MAX];
    size_t capacity;
    uint8_t *dst;

    snprintf(root, sizeof(root), "%s/rk-no-signal-XXXXXX",
             getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(root) != NULL);
    assert(setenv("ONEKVM_NO_SIGNAL_CACHE", root, 1) == 0);
    memset(&zero, 0, sizeof(zero));

    /* No cache yet: limited-range black and an empty token. */
    dst = frame(656, 480, 480, &capacity);
    assert(dst != NULL);
    assert(rk_no_signal_render_nv12(dst, capacity, 640, 480, 656, 480) == 0);
    expect_luma(dst, 656, 0, 0, 16);
    expect_luma(dst, 656, 655, 479, 16);
    expect_chroma(dst, 656, 480, 320, 240, 128, 128);
    rk_no_signal_token(640, 480, &none);
    assert(memcmp(&none, &zero, sizeof(zero)) == 0);

    /* The exact size is copied; NV21 VU becomes NV12 UV. */
    path_of(path, sizeof(path), "zh");
    assert(mkdir(path, 0755) == 0);
    path_of(path, sizeof(path), "en");
    assert(mkdir(path, 0755) == 0);
    write_nv21("zh/640x480.nv21", 640, 480, 200, 10, 240, 0);
    write_nv21("en/640x480.nv21", 640, 480, 90, 60, 160, 0);
    point_current("zh");
    assert(rk_no_signal_render_nv12(dst, capacity, 640, 480, 656, 480) == 0);
    expect_luma(dst, 656, 0, 0, 200);
    expect_luma(dst, 656, 639, 479, 200);
    expect_luma(dst, 656, 650, 10, 16);
    expect_chroma(dst, 656, 480, 0, 0, 240, 10);
    expect_chroma(dst, 656, 480, 638, 478, 240, 10);
    rk_no_signal_token(640, 480, &zh);
    assert(memcmp(&zh, &zero, sizeof(zero)) != 0);

    /* A language switch moves current: the token changes. */
    point_current("en");
    rk_no_signal_token(640, 480, &en);
    assert(memcmp(&en, &zh, sizeof(zh)) != 0);
    assert(rk_no_signal_render_nv12(dst, capacity, 640, 480, 656, 480) == 0);
    expect_luma(dst, 656, 320, 240, 90);
    expect_chroma(dst, 656, 480, 320, 240, 160, 60);
    free(dst);

    /* The nearest cached size keeps its aspect: 640x480 in 800x480 gets side bars. */
    dst = frame(800, 480, 480, &capacity);
    assert(dst != NULL);
    assert(rk_no_signal_render_nv12(dst, capacity, 800, 480, 800, 480) == 0);
    expect_luma(dst, 800, 79, 100, 16);
    expect_luma(dst, 800, 80, 100, 90);
    expect_luma(dst, 800, 719, 100, 90);
    expect_luma(dst, 800, 720, 100, 16);
    expect_chroma(dst, 800, 480, 78, 100, 128, 128);
    expect_chroma(dst, 800, 480, 80, 100, 160, 60);
    expect_chroma(dst, 800, 480, 720, 100, 128, 128);
    free(dst);

    /* 1280x720 in 1280x1024 gets bars above and below. */
    write_nv21("en/1280x720.nv21", 1280, 720, 120, 70, 180, 0);
    dst = frame(1280, 1024, 1024, &capacity);
    assert(dst != NULL);
    assert(rk_no_signal_render_nv12(dst, capacity, 1280, 1024, 1280, 1024) == 0);
    expect_luma(dst, 1280, 640, 151, 16);
    expect_luma(dst, 1280, 640, 152, 120);
    expect_luma(dst, 1280, 640, 871, 120);
    expect_luma(dst, 1280, 640, 872, 16);
    expect_chroma(dst, 1280, 1024, 640, 152, 180, 70);
    expect_chroma(dst, 1280, 1024, 640, 150, 128, 128);
    free(dst);

    /* A truncated file is not drawn. */
    write_nv21("en/1920x1080.nv21", 1920, 1080, 220, 1, 2, 1);
    dst = frame(1920, 1080, 1080, &capacity);
    assert(dst != NULL);
    assert(rk_no_signal_render_nv12(dst, capacity, 1920, 1080, 1920, 1080) == 0);
    expect_luma(dst, 1920, 960, 540, 16);

    /* Invalid frames are rejected. */
    assert(rk_no_signal_render_nv12(dst, capacity - 1, 1920, 1080, 1920, 1080) == -1);
    assert(rk_no_signal_render_nv12(dst, capacity, 1919, 1080, 1920, 1080) == -1);
    assert(rk_no_signal_render_nv12(dst, capacity, 1920, 1080, 1900, 1080) == -1);
    assert(rk_no_signal_render_nv12(NULL, capacity, 1920, 1080, 1920, 1080) == -1);
    free(dst);

    path_of(path, sizeof(path), "en/1920x1080.nv21");
    assert(unlink(path) == 0);
    path_of(path, sizeof(path), "en/1280x720.nv21");
    assert(unlink(path) == 0);
    path_of(path, sizeof(path), "en/640x480.nv21");
    assert(unlink(path) == 0);
    path_of(path, sizeof(path), "zh/640x480.nv21");
    assert(unlink(path) == 0);
    path_of(path, sizeof(path), "current");
    assert(unlink(path) == 0);
    path_of(path, sizeof(path), "en");
    assert(rmdir(path) == 0);
    path_of(path, sizeof(path), "zh");
    assert(rmdir(path) == 0);
    assert(rmdir(root) == 0);
    puts("no-signal: ok");
    return 0;
}
