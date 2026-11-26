/* SPDX-License-Identifier: MIT */

#include "v4l2_mplane.h"
#include "internal.h"
#include "display_csc.h"
#include "rga_scale.h"
#include "snapshot.h"
#include "edid_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RK_SOURCE_MAX_SUBDEV 8

struct rk_slot {
    void *start;
    size_t length;
    bool outstanding;
};

#define RK_DISPLAY_W 640
#define RK_DISPLAY_H 360
#define RK_DISPLAY_MIN_NS 66000000ull
/* How long the display reuses the bridge YCbCr matrix and range. */
#define RK_DISPLAY_CSC_NS 1000000000ull
/* QUERY_DV_TIMINGS is about 14 TC358743 register reads on a 100 kHz I2C
 * bus. The bound capture thread refreshes timings every 200 ms; status
 * queries reuse that result while it is this fresh. */
#define RK_TIMINGS_CACHE_NS 500000000ull

struct rk_source {
    pthread_mutex_t mutex;
    pthread_cond_t display_cv;
    char device[256];
    char subdev[64];
    int video_fd;
    int subdev_fd;
    int interrupt_fd;
    int32_t want_width;
    int32_t want_height;
    int32_t width;
    int32_t height;
    int32_t fps;
    uint32_t fourcc;
    uint32_t bytesperline;
    bool signal;
    bool streaming;
    unsigned skip_dq;
    unsigned n_buffers;
    struct rk_slot buffers[ONEKVM_RK_MPP_BUFFER_COUNT];
    uint64_t last_capture_ns;
    uint8_t *display_rgb;
    int32_t display_w;
    int32_t display_h;
    uint64_t display_pts;
    uint32_t display_seq;
    uint32_t display_read_seq;
    uint64_t display_publish_ns;
    uint64_t display_csc_ns;
    uint32_t display_encoding;
    uint32_t display_quantization;
    bool display_rga;
    void *capture_owner;
    /* width:16 height:16 fps:16 signal:1, published after each query. */
    atomic_uint_least64_t timings_cache;
    atomic_uint_least64_t timings_ns;
};

bool rk_source_capture_active(void *opaque)
{
    struct rk_source *source = opaque;
    bool active;
    pthread_mutex_lock(&source->mutex);
    active = source->capture_owner != NULL;
    pthread_mutex_unlock(&source->mutex);
    return active;
}

bool rk_source_capture_claim(void *opaque, void *encoder)
{
    struct rk_source *source = opaque;
    bool claimed;
    pthread_mutex_lock(&source->mutex);
    claimed = source->capture_owner == NULL || source->capture_owner == encoder;
    if (claimed) {
        source->capture_owner = encoder;
    }
    pthread_mutex_unlock(&source->mutex);
    return claimed;
}

void rk_source_capture_release(void *opaque, void *encoder)
{
    struct rk_source *source = opaque;
    pthread_mutex_lock(&source->mutex);
    if (source->capture_owner == encoder) {
        source->capture_owner = NULL;
    }
    pthread_mutex_unlock(&source->mutex);
}

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;
    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int open_subdev_for_timings(char *path, size_t path_size)
{
    DIR *dir;
    struct dirent *ent;
    int found = -1;

    dir = opendir("/dev");
    if (dir == NULL) {
        return -1;
    }
    while ((ent = readdir(dir)) != NULL) {
        int fd;
        struct v4l2_dv_timings timings;
        char candidate[64];

        if (strncmp(ent->d_name, "v4l-subdev", 10) != 0) {
            continue;
        }
        if (snprintf(candidate, sizeof(candidate), "/dev/%s", ent->d_name) >=
            (int)sizeof(candidate)) {
            continue;
        }
        fd = open(candidate, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        memset(&timings, 0, sizeof(timings));
        if (xioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &timings) == 0 || errno == ENOLINK ||
            errno == ENOLCK || errno == ERANGE) {
            if (path != NULL && path_size > 0) {
                snprintf(path, path_size, "%s", candidate);
            }
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(dir);
    return found;
}

static void disable_hdmi_sleep(int subdev_fd)
{
    char sysfs[256];
    char buf[8];
    ssize_t n;
    int fd;

    (void)subdev_fd;
    fd = open(
        "/sys/devices/platform/ff470000.i2c/i2c-4/4-000f/sleep_mode",
        O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0 && buf[0] != '0') {
        lseek(fd, 0, SEEK_SET);
        (void)write(fd, "0", 1);
    }
    close(fd);
    (void)sysfs;
}

/* Reads the bridge EDID; 0 when it has none. */
static size_t read_bridge_edid(int fd, uint8_t *edid, size_t capacity)
{
    struct v4l2_edid query;

    memset(&query, 0, sizeof(query));
    query.pad = 0;
    query.blocks = (uint32_t)(capacity / 128);
    query.edid = edid;
    if (xioctl(fd, VIDIOC_G_EDID, &query) != 0) {
        return 0;
    }
    return (size_t)query.blocks * 128u;
}

static int write_bridge_edid(int fd, const uint8_t *edid, size_t size)
{
    struct v4l2_edid query;
    uint8_t copy[RK_EDID_MAX_BYTES];

    memcpy(copy, edid, size);
    memset(&query, 0, sizeof(query));
    query.pad = 0;
    query.blocks = (uint32_t)(size / 128);
    query.edid = copy;
    return xioctl(fd, VIDIOC_S_EDID, &query);
}

/*
 * Load this boot's EDID: the one saved by a permanent write, otherwise the
 * factory EDID. Every write replugs the input, so after the first source of
 * the boot only a bridge with no EDID or the driver's own one is written;
 * an EDID set through the EDID API, temporary or not, outlives the source.
 */
static void restore_boot_edid(int fd)
{
    const struct rk_edid_paths *paths = &rk_edid_default_paths;
    uint8_t current[RK_EDID_MAX_BYTES];
    uint8_t wanted[RK_EDID_MAX_BYTES];
    size_t current_size;
    size_t wanted_size;
    bool restored;

    if (fd < 0) {
        return;
    }
    restored = rk_edid_restored(paths);
    current_size = read_bridge_edid(fd, current, sizeof(current));
    wanted_size = rk_edid_boot_choice(paths, wanted, sizeof(wanted));
    if (rk_edid_needs_write(current, current_size, wanted, wanted_size, restored)) {
        if (write_bridge_edid(fd, wanted, wanted_size) != 0) {
            return;
        }
        usleep(200000);
    }
    if (!restored) {
        rk_edid_mark_restored(paths);
    }
}

static void store_dv_timings(struct rk_source *source, const struct v4l2_dv_timings *timings)
{
    double fps;

    source->width = (int32_t)timings->bt.width;
    source->height = (int32_t)timings->bt.height;
    fps = (double)timings->bt.pixelclock /
          ((double)(timings->bt.height + timings->bt.vfrontporch + timings->bt.vsync +
                     timings->bt.vbackporch) *
           (double)(timings->bt.width + timings->bt.hfrontporch + timings->bt.hsync +
                     timings->bt.hbackporch));
    source->fps = (int32_t)(fps + 0.5);
    if (source->fps < 1) {
        source->fps = 1;
    }
    source->signal = source->width > 0 && source->height > 0;
}

static void publish_timings_locked(struct rk_source *source)
{
    uint64_t packed = ((uint64_t)(uint16_t)source->width << 48) |
                      ((uint64_t)(uint16_t)source->height << 32) |
                      ((uint64_t)(uint16_t)source->fps << 16) | (source->signal ? 1u : 0u);

    atomic_store(&source->timings_cache, packed);
    atomic_store(&source->timings_ns, monotonic_ns());
}

/* Lock-free read of the last query, so per-packet metadata never waits for
 * the capture thread's I2C round trip on source->mutex. */
static bool cached_timings(struct rk_source *source, int32_t *width, int32_t *height,
                           int32_t *fps, bool *signal)
{
    uint64_t at = atomic_load(&source->timings_ns);
    uint64_t packed;

    if (at == 0 || monotonic_ns() - at >= RK_TIMINGS_CACHE_NS) {
        return false;
    }
    packed = atomic_load(&source->timings_cache);
    *width = (int32_t)(uint16_t)(packed >> 48);
    *height = (int32_t)(uint16_t)(packed >> 32);
    *fps = (int32_t)(uint16_t)(packed >> 16);
    *signal = (packed & 1u) != 0;
    return true;
}

static int query_dv_timings_locked(struct rk_source *source, struct v4l2_dv_timings *timings)
{
    if (source->subdev_fd < 0) {
        source->signal = false;
        publish_timings_locked(source);
        return -1;
    }
    disable_hdmi_sleep(source->subdev_fd);
    memset(timings, 0, sizeof(*timings));
    if (xioctl(source->subdev_fd, VIDIOC_QUERY_DV_TIMINGS, timings) != 0) {
        source->signal = false;
        publish_timings_locked(source);
        return -1;
    }
    store_dv_timings(source, timings);
    publish_timings_locked(source);
    return 0;
}

static void query_timings_locked(struct rk_source *source)
{
    struct v4l2_dv_timings timings;

    /* Poll path: QUERY only. S_DV_TIMINGS on a mismatch calls enable_stream(false)
     * and never re-enables CSI. Pixelclock jitter from QUERY would then kill HS. */
    (void)query_dv_timings_locked(source, &timings);
}

static void apply_detected_timings_locked(struct rk_source *source)
{
    struct v4l2_dv_timings timings;

    if (query_dv_timings_locked(source, &timings) != 0) {
        return;
    }
    (void)xioctl(source->subdev_fd, VIDIOC_S_DV_TIMINGS, &timings);
}

static void unmap_buffers(struct rk_source *source)
{
    unsigned i;

    for (i = 0; i < ONEKVM_RK_MPP_BUFFER_COUNT; i++) {
        if (source->buffers[i].start != NULL && source->buffers[i].start != MAP_FAILED) {
            munmap(source->buffers[i].start, source->buffers[i].length);
        }
        source->buffers[i].start = NULL;
        source->buffers[i].length = 0;
        source->buffers[i].outstanding = false;
    }
    source->n_buffers = 0;
}

static void stop_mmap_capture(struct rk_source *source)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_requestbuffers req;

    if (source->video_fd < 0) {
        return;
    }
    if (source->streaming) {
        xioctl(source->video_fd, VIDIOC_STREAMOFF, &type);
        source->streaming = false;
    }
    unmap_buffers(source);
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(source->video_fd, VIDIOC_REQBUFS, &req);
    close(source->video_fd);
    source->video_fd = -1;
}

static bool video_is_cif_capture(const char *path)
{
    struct v4l2_capability cap;
    uint32_t caps;
    int fd;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        close(fd);
        return false;
    }
    close(fd);
    caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0 ? cap.device_caps : cap.capabilities;
    if ((caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0) {
        return false;
    }
    return (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
}

static void resolve_cif_device(char *out, size_t n, const char *preferred)
{
    DIR *dir;
    struct dirent *ent;

    if (preferred != NULL && preferred[0] != '\0' && video_is_cif_capture(preferred)) {
        snprintf(out, n, "%s", preferred);
        return;
    }
    dir = opendir("/sys/class/video4linux");
    if (dir != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            char sysname[128];
            char name[64];
            FILE *fp;

            if (strncmp(ent->d_name, "video", 5) != 0) {
                continue;
            }
            if (snprintf(sysname, sizeof(sysname), "/sys/class/video4linux/%s/name",
                         ent->d_name) >= (int)sizeof(sysname)) {
                continue;
            }
            fp = fopen(sysname, "r");
            if (fp == NULL) {
                continue;
            }
            if (fgets(name, sizeof(name), fp) == NULL) {
                fclose(fp);
                continue;
            }
            fclose(fp);
            if (strncmp(name, "stream_cif_mipi_id0", 19) != 0) {
                continue;
            }
            snprintf(out, n, "/dev/%s", ent->d_name);
            fprintf(stderr, "rk-mpp: CIF %s (preferred %s was not capture)\n", out,
                    preferred != NULL ? preferred : "-");
            closedir(dir);
            return;
        }
        closedir(dir);
    }
    snprintf(out, n, "%s", preferred != NULL && preferred[0] != '\0' ? preferred
                                                                      : ONEKVM_RK_MPP_DEFAULT_DEVICE);
}

static bool try_s_fmt(int fd, uint32_t fourcc, int32_t width, int32_t height,
                      struct v4l2_format *fmt)
{
    memset(fmt, 0, sizeof(*fmt));
    fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt->fmt.pix_mp.width = (uint32_t)width;
    fmt->fmt.pix_mp.height = (uint32_t)height;
    fmt->fmt.pix_mp.pixelformat = fourcc;
    fmt->fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt->fmt.pix_mp.num_planes = 1;
    if (height > 576) {
        fmt->fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
        fmt->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_709;
    } else {
        fmt->fmt.pix_mp.colorspace = V4L2_COLORSPACE_SMPTE170M;
        fmt->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_601;
    }
    fmt->fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE;
    fmt->fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
    return xioctl(fd, VIDIOC_S_FMT, fmt) == 0;
}

static int apply_subdev_format_locked(struct rk_source *source, int32_t width, int32_t height)
{
    struct v4l2_subdev_format sfmt;

    if (source->subdev_fd < 0) {
        return -1;
    }
    /* Pad format only. S_DV_TIMINGS here (called 3× from capture_drain)
     * enable_stream(false) and can leave CSI off — CIF size-err, dq=0. */
    memset(&sfmt, 0, sizeof(sfmt));
    sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    /* Keep the bridge's own YUV422 code: TC358743 UYVY8_1X16, RK628 UYVY8_2X8. */
    if (xioctl(source->subdev_fd, VIDIOC_SUBDEV_G_FMT, &sfmt) != 0 || sfmt.format.code == 0) {
        sfmt.format.code = MEDIA_BUS_FMT_UYVY8_1X16;
    }
    sfmt.format.width = (uint32_t)width;
    sfmt.format.height = (uint32_t)height;
    sfmt.format.field = V4L2_FIELD_NONE;
    return xioctl(source->subdev_fd, VIDIOC_SUBDEV_S_FMT, &sfmt);
}

int rk_source_apply_subdev_format(void *opaque, int32_t width, int32_t height)
{
    struct rk_source *source = opaque;
    int ret;

    if (source == NULL) {
        return -1;
    }
    pthread_mutex_lock(&source->mutex);
    ret = apply_subdev_format_locked(source, width, height);
    pthread_mutex_unlock(&source->mutex);
    return ret;
}

static int source_ycbcr_locked(struct rk_source *source, uint32_t *encoding,
                               uint32_t *quantization)
{
    struct v4l2_subdev_format sfmt;
    int ret = -1;

    memset(&sfmt, 0, sizeof(sfmt));
    sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    if (source->subdev_fd >= 0 &&
        xioctl(source->subdev_fd, VIDIOC_SUBDEV_G_FMT, &sfmt) == 0 &&
        (sfmt.format.code == MEDIA_BUS_FMT_UYVY8_1X16 ||
         sfmt.format.code == MEDIA_BUS_FMT_UYVY8_2X8) &&
        sfmt.format.colorspace != V4L2_COLORSPACE_DEFAULT) {
        /* TC358743 reports only the colourspace; resolve the defaults.
         * RK628 reports the full-range BT.709 or BT.2020 its CSC writes. */
        *encoding = sfmt.format.ycbcr_enc != V4L2_YCBCR_ENC_DEFAULT
                        ? sfmt.format.ycbcr_enc
                        : V4L2_MAP_YCBCR_ENC_DEFAULT(sfmt.format.colorspace);
        *quantization = sfmt.format.quantization != V4L2_QUANTIZATION_DEFAULT
                            ? sfmt.format.quantization
                            : V4L2_MAP_QUANTIZATION_DEFAULT(false, sfmt.format.colorspace,
                                                            *encoding);
        ret = 0;
    }
    return ret;
}

int rk_source_ycbcr(void *opaque, uint32_t *encoding, uint32_t *quantization)
{
    struct rk_source *source = opaque;
    int ret;

    if (source == NULL || encoding == NULL || quantization == NULL) {
        return -1;
    }
    pthread_mutex_lock(&source->mutex);
    ret = source_ycbcr_locked(source, encoding, quantization);
    pthread_mutex_unlock(&source->mutex);
    return ret;
}

int rk_source_lock_detected_timings(void *opaque)
{
    struct rk_source *source = opaque;

    if (source == NULL) {
        return -1;
    }
    pthread_mutex_lock(&source->mutex);
    apply_detected_timings_locked(source);
    pthread_mutex_unlock(&source->mutex);
    return source->signal ? 0 : -1;
}

static int32_t start_mmap_capture(struct rk_source *source, char *error, uint32_t error_capacity)
{
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct v4l2_plane plane;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    unsigned i;
    int32_t width;
    int32_t height;

    stop_mmap_capture(source);
    /* JetKVM kvm_video waits for QUERY_DV_TIMINGS before STREAMON. */
    {
        int tries;
        for (tries = 0; tries < 20; tries++) {
            apply_detected_timings_locked(source);
            if (source->signal && source->width > 0 && source->height > 0) {
                break;
            }
            pthread_mutex_unlock(&source->mutex);
            usleep(50000);
            pthread_mutex_lock(&source->mutex);
        }
    }
    width = source->width;
    height = source->height;
    if (width <= 0 || height <= 0) {
        width = 1920;
        height = 1080;
    }
    (void)apply_subdev_format_locked(source, width, height);
    source->video_fd = open(source->device, O_RDWR | O_CLOEXEC);
    if (source->video_fd < 0) {
        onekvm_set_error(error, error_capacity, "open %s: %s", source->device, strerror(errno));
        return -1;
    }
    if (rk_rga_present()) {
        if (!try_s_fmt(source->video_fd, V4L2_PIX_FMT_NV16, width, height, &fmt) &&
            !try_s_fmt(source->video_fd, V4L2_PIX_FMT_YUYV, width, height, &fmt)) {
            onekvm_set_error(error, error_capacity, "VIDIOC_S_FMT NV16/YUYV failed: %s",
                             strerror(errno));
            close(source->video_fd);
            source->video_fd = -1;
            return -1;
        }
    } else if (!try_s_fmt(source->video_fd, V4L2_PIX_FMT_YUYV, width, height, &fmt) &&
               !try_s_fmt(source->video_fd, V4L2_PIX_FMT_NV16, width, height, &fmt)) {
        onekvm_set_error(error, error_capacity, "VIDIOC_S_FMT YUYV/NV16 failed: %s",
                         strerror(errno));
        close(source->video_fd);
        source->video_fd = -1;
        return -1;
    }
    source->fourcc = fmt.fmt.pix_mp.pixelformat;
    source->width = (int32_t)fmt.fmt.pix_mp.width;
    source->height = (int32_t)fmt.fmt.pix_mp.height;
    source->bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    memset(&req, 0, sizeof(req));
    req.count = ONEKVM_RK_MPP_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(source->video_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        onekvm_set_error(error, error_capacity, "VIDIOC_REQBUFS: %s", strerror(errno));
        close(source->video_fd);
        source->video_fd = -1;
        return -1;
    }
    for (i = 0; i < req.count && i < ONEKVM_RK_MPP_BUFFER_COUNT; i++) {
        memset(&buf, 0, sizeof(buf));
        memset(&plane, 0, sizeof(plane));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = &plane;
        buf.length = 1;
        if (xioctl(source->video_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            onekvm_set_error(error, error_capacity, "VIDIOC_QUERYBUF: %s", strerror(errno));
            stop_mmap_capture(source);
            return -1;
        }
        source->buffers[i].length = plane.length;
        source->buffers[i].start =
            mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, source->video_fd,
                 plane.m.mem_offset);
        if (source->buffers[i].start == MAP_FAILED) {
            onekvm_set_error(error, error_capacity, "mmap: %s", strerror(errno));
            source->buffers[i].start = NULL;
            stop_mmap_capture(source);
            return -1;
        }
        if (xioctl(source->video_fd, VIDIOC_QBUF, &buf) < 0) {
            onekvm_set_error(error, error_capacity, "VIDIOC_QBUF: %s", strerror(errno));
            stop_mmap_capture(source);
            return -1;
        }
        source->n_buffers++;
    }
    if (xioctl(source->video_fd, VIDIOC_STREAMON, &type) < 0) {
        onekvm_set_error(error, error_capacity, "VIDIOC_STREAMON: %s", strerror(errno));
        stop_mmap_capture(source);
        return -1;
    }
    source->streaming = true;
    source->skip_dq = 2;
    return 0;
}

static int32_t configure_source(struct rk_source *source,
                                const struct onekvm_video_source_config_v1 *config, char *error,
                                uint32_t error_capacity)
{
    resolve_cif_device(source->device, sizeof(source->device), onekvm_device_path(config));
    if (config->resolution != 0 &&
        !onekvm_map_resolution(config->resolution, &source->want_width, &source->want_height)) {
        onekvm_set_error(error, error_capacity, "unsupported resolution %d", config->resolution);
        return -1;
    }
    if (config->resolution == 0) {
        source->want_width = 0;
        source->want_height = 0;
    }
    if (source->subdev_fd < 0) {
        source->subdev_fd = open_subdev_for_timings(source->subdev, sizeof(source->subdev));
    }
    if (source->subdev_fd >= 0) {
        disable_hdmi_sleep(source->subdev_fd);
        restore_boot_edid(source->subdev_fd);
    }
    apply_detected_timings_locked(source);
    return 0;
}

int32_t rk_source_create(const struct onekvm_video_source_config_v1 *config, void **out, char *error,
                         uint32_t error_capacity)
{
    struct rk_source *source;

    if (out != NULL) {
        *out = NULL;
    }
    if (config == NULL || out == NULL ||
        config->struct_size < sizeof(struct onekvm_video_source_config_v1)) {
        onekvm_set_error(error, error_capacity, "invalid video source config");
        return -1;
    }
    source = calloc(1, sizeof(*source));
    if (source == NULL) {
        onekvm_set_error(error, error_capacity, "allocate source: out of memory");
        return -1;
    }
    pthread_mutex_init(&source->mutex, NULL);
    atomic_init(&source->timings_cache, 0);
    atomic_init(&source->timings_ns, 0);
    {
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&source->display_cv, &attr);
        pthread_condattr_destroy(&attr);
    }
    source->video_fd = -1;
    source->subdev_fd = -1;
    source->interrupt_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (source->interrupt_fd < 0) {
        onekvm_set_error(error, error_capacity, "eventfd: %s", strerror(errno));
        pthread_cond_destroy(&source->display_cv);
        pthread_mutex_destroy(&source->mutex);
        free(source);
        return -1;
    }
    if (configure_source(source, config, error, error_capacity) != 0) {
        close(source->interrupt_fd);
        pthread_cond_destroy(&source->display_cv);
        pthread_mutex_destroy(&source->mutex);
        free(source);
        return -1;
    }
    *out = source;
    return 0;
}

int32_t rk_source_reset(void *opaque, const struct onekvm_video_source_config_v1 *config,
                        char *error, uint32_t error_capacity)
{
    struct rk_source *source = opaque;

    int width = 0;
    int height = 0;

    if (source == NULL || config == NULL || config->struct_size < sizeof(*config)) {
        onekvm_set_error(error, error_capacity, "invalid source reset config");
        return -1;
    }
    if (config->resolution != 0 &&
        !onekvm_map_resolution(config->resolution, &width, &height)) {
        onekvm_set_error(error, error_capacity,
                         "unsupported output size: at most 1920x1080, width a multiple of 16, even height");
        return ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
    }
    /* Validation must leave everything unchanged on failure: Core calls this
     * while the encoder is still bound (SOURCE_RESET_WHILE_BOUND). The new
     * size reaches the encoder when it reopens VEPU on the next bind. */
    pthread_mutex_lock(&source->mutex);
    if (width != 0 && source->signal &&
        ((width != source->width || height != source->height) &&
         (!onekvm_scale_supported(source->width, source->height, width, height) ||
          !rk_rga_present()))) {
        int input_width = source->width;
        int input_height = source->height;

        pthread_mutex_unlock(&source->mutex);
        onekvm_set_error(error, error_capacity,
                         "unsupported output size %dx%d for HDMI input %dx%d: downscale only, at most 8:1",
                         width, height, input_width, input_height);
        return ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
    }
    stop_mmap_capture(source);
    source->want_width = width;
    source->want_height = height;
    pthread_mutex_unlock(&source->mutex);
    return 0;
}

int rk_source_output_size(void *opaque, int width, int height, int *dw, int *dh)
{
    struct rk_source *source = opaque;
    pthread_mutex_lock(&source->mutex);
    *dw = source->want_width > 0 ? source->want_width : width;
    *dh = source->want_height > 0 ? source->want_height : height;
    pthread_mutex_unlock(&source->mutex);
    if (*dw == width && *dh == height) {
        return 0;
    }
    return onekvm_scale_supported(width, height, *dw, *dh) ? 1 : -1;
}

int32_t rk_source_read(void *opaque, struct onekvm_video_frame_v1 *frame, char *error,
                       uint32_t error_capacity)
{
    struct rk_source *source = opaque;
    struct pollfd fds[2];
    struct v4l2_buffer buf;
    struct v4l2_plane plane;
    uint64_t token;
    uint64_t capture_start_ns;

    if (source == NULL || frame == NULL || frame->struct_size < sizeof(*frame)) {
        onekvm_set_error(error, error_capacity, "invalid source read");
        return -1;
    }
    pthread_mutex_lock(&source->mutex);
    if (!source->streaming && start_mmap_capture(source, error, error_capacity) != 0) {
        pthread_mutex_unlock(&source->mutex);
        return -1;
    }
    memset(fds, 0, sizeof(fds));
    fds[0].fd = source->video_fd;
    fds[0].events = POLLIN;
    fds[1].fd = source->interrupt_fd;
    fds[1].events = POLLIN;
    pthread_mutex_unlock(&source->mutex);

    capture_start_ns = monotonic_ns();
    if (poll(fds, 2, 1000) <= 0) {
        onekvm_set_error(error, error_capacity, "capture timed out");
        return ONEKVM_VIDEO_RESOURCE_TIMEOUT;
    }
    if (fds[1].revents & POLLIN) {
        uint64_t value;
        (void)read(source->interrupt_fd, &value, sizeof(value));
        onekvm_set_error(error, error_capacity, "capture interrupted");
        return -1;
    }

    pthread_mutex_lock(&source->mutex);
    memset(&buf, 0, sizeof(buf));
    memset(&plane, 0, sizeof(plane));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = &plane;
    buf.length = 1;
    if (xioctl(source->video_fd, VIDIOC_DQBUF, &buf) < 0) {
        onekvm_set_error(error, error_capacity, "VIDIOC_DQBUF: %s", strerror(errno));
        pthread_mutex_unlock(&source->mutex);
        return -1;
    }
    if (source->skip_dq > 0) {
        source->skip_dq--;
        if (xioctl(source->video_fd, VIDIOC_QBUF, &buf) < 0) {
            onekvm_set_error(error, error_capacity, "VIDIOC_QBUF: %s", strerror(errno));
            pthread_mutex_unlock(&source->mutex);
            return -1;
        }
        pthread_mutex_unlock(&source->mutex);
        return ONEKVM_VIDEO_RESOURCE_TIMEOUT;
    }
    onekvm_store_stage_ns(&source->last_capture_ns, monotonic_ns() - capture_start_ns);
    token = (uint64_t)buf.index + 1;
    source->buffers[buf.index].outstanding = true;
    frame->data = source->buffers[buf.index].start;
    frame->data_size = plane.bytesused;
    frame->width = source->width;
    frame->height = source->height;
    frame->pixel_format = ONEKVM_VIDEO_PIXEL_UNKNOWN;
    frame->pts_ns = monotonic_ns();
    frame->token = token;
    pthread_mutex_unlock(&source->mutex);
    return 0;
}

void rk_source_release(void *opaque, uint64_t token)
{
    struct rk_source *source = opaque;
    struct v4l2_buffer buf;
    struct v4l2_plane plane;
    unsigned index;

    if (source == NULL || token == 0) {
        return;
    }
    index = (unsigned)(token - 1);
    pthread_mutex_lock(&source->mutex);
    if (source->video_fd >= 0 && index < source->n_buffers &&
        source->buffers[index].outstanding) {
        memset(&buf, 0, sizeof(buf));
        memset(&plane, 0, sizeof(plane));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = index;
        buf.m.planes = &plane;
        buf.length = 1;
        (void)xioctl(source->video_fd, VIDIOC_QBUF, &buf);
        source->buffers[index].outstanding = false;
    }
    pthread_mutex_unlock(&source->mutex);
}

int32_t rk_source_signal_present(void *opaque)
{
    struct rk_source *source = opaque;
    int32_t present;

    if (source == NULL) {
        return 0;
    }
    {
        int32_t width;
        int32_t height;
        int32_t fps;
        bool signal;

        if (cached_timings(source, &width, &height, &fps, &signal)) {
            return signal ? 1 : 0;
        }
    }
    pthread_mutex_lock(&source->mutex);
    query_timings_locked(source);
    present = source->signal ? 1 : 0;
    pthread_mutex_unlock(&source->mutex);
    return present;
}

void rk_source_interrupt(void *opaque)
{
    struct rk_source *source = opaque;
    uint64_t one = 1;

    if (source == NULL || source->interrupt_fd < 0) {
        return;
    }
    (void)write(source->interrupt_fd, &one, sizeof(one));
}

void rk_source_destroy(void *opaque)
{
    struct rk_source *source = opaque;

    if (source == NULL) {
        return;
    }
    rk_snapshot_quiesce(source);
    pthread_mutex_lock(&source->mutex);
    stop_mmap_capture(source);
    if (source->subdev_fd >= 0) {
        close(source->subdev_fd);
        source->subdev_fd = -1;
    }
    pthread_mutex_unlock(&source->mutex);
    if (source->interrupt_fd >= 0) {
        close(source->interrupt_fd);
    }
    pthread_cond_destroy(&source->display_cv);
    free(source->display_rgb);
    pthread_mutex_destroy(&source->mutex);
    free(source);
}

int32_t rk_source_latency(void *opaque, struct onekvm_video_latency_v1 *latency)
{
    struct rk_source *source = opaque;

    if (source == NULL) {
        return -1;
    }
    return onekvm_fill_latency(latency, onekvm_load_stage_ns(&source->last_capture_ns), 0);
}

void rk_source_note_capture_ns(void *opaque, uint64_t capture_ns)
{
    struct rk_source *source = opaque;

    if (source == NULL) {
        return;
    }
    onekvm_store_stage_ns(&source->last_capture_ns, capture_ns);
}

static void rk_scale_yuyv(const uint8_t *src, int32_t sw, int32_t sh, uint32_t stride, uint16_t *dst,
                          int32_t dw, int32_t dh, const struct rk_yuv_coef *k)
{
    int32_t y;
    int32_t x;

    for (y = 0; y < dh; y++) {
        int32_t sy = (int32_t)(((int64_t)y * sh) / dh);
        const uint8_t *row = src + (size_t)sy * stride;

        for (x = 0; x < dw; x++) {
            int32_t sx = (int32_t)(((int64_t)x * sw) / dw);
            const uint8_t *p = row + (size_t)(sx & ~1) * 2u;
            int yy = row[(size_t)sx * 2u];
            dst[(size_t)y * (size_t)dw + (size_t)x] = rk_yuv_rgb565(k, yy, p[1], p[3]);
        }
    }
}

static void rk_scale_nv16(const uint8_t *src, int32_t sw, int32_t sh, uint32_t stride, uint16_t *dst,
                          int32_t dw, int32_t dh, const struct rk_yuv_coef *k)
{
    const uint8_t *uv = src + (size_t)stride * (size_t)sh;
    int32_t y;
    int32_t x;

    for (y = 0; y < dh; y++) {
        int32_t sy = (int32_t)(((int64_t)y * sh) / dh);
        const uint8_t *yrow = src + (size_t)sy * stride;
        const uint8_t *uvrow = uv + (size_t)sy * stride;

        for (x = 0; x < dw; x++) {
            int32_t sx = (int32_t)(((int64_t)x * sw) / dw);
            dst[(size_t)y * (size_t)dw + (size_t)x] =
                rk_yuv_rgb565(k, yrow[sx], uvrow[(sx & ~1)], uvrow[(sx & ~1) + 1]);
        }
    }
}

void rk_source_publish_display(void *opaque, const uint8_t *src, int src_fd, uint32_t fourcc,
                               int32_t width, int32_t height, uint32_t stride, uint64_t pts_ns)
{
    struct rk_source *source = opaque;
    size_t need;
    uint64_t now;
    bool used_rga = false;

    if (source == NULL || width <= 0 || height <= 0 || stride == 0) {
        return;
    }
    if (src == NULL && src_fd < 0) {
        return;
    }
    now = monotonic_ns();
    pthread_mutex_lock(&source->mutex);
    if (source->display_publish_ns != 0 && now > source->display_publish_ns &&
        now - source->display_publish_ns < RK_DISPLAY_MIN_NS) {
        pthread_mutex_unlock(&source->mutex);
        return;
    }
    if (source->display_seq != 0 && source->display_seq != source->display_read_seq) {
        pthread_mutex_unlock(&source->mutex);
        return;
    }
    need = (size_t)RK_DISPLAY_W * (size_t)RK_DISPLAY_H * 2u;
    if (source->display_rgb == NULL) {
        source->display_rgb = malloc(need);
        if (source->display_rgb == NULL) {
            pthread_mutex_unlock(&source->mutex);
            return;
        }
    }
    /* TC358743 writes BT.601 limited range, RK628F full-range BT.709. */
    if (source->display_csc_ns == 0 || now - source->display_csc_ns >= RK_DISPLAY_CSC_NS) {
        if (source_ycbcr_locked(source, &source->display_encoding,
                                &source->display_quantization) != 0) {
            source->display_encoding = V4L2_YCBCR_ENC_DEFAULT;
            source->display_quantization = V4L2_QUANTIZATION_DEFAULT;
        }
        source->display_csc_ns = now;
    }
    if (src_fd >= 0 &&
        rk_rga_scale_rgb565(src_fd, fourcc, width, height, stride, source->display_encoding,
                            source->display_quantization, (uint16_t *)source->display_rgb,
                            RK_DISPLAY_W, RK_DISPLAY_H) == 0) {
        used_rga = true;
    } else if (src != NULL) {
        const struct rk_yuv_coef *k =
            rk_yuv_coef_for(source->display_encoding, source->display_quantization);

        if (fourcc == V4L2_PIX_FMT_NV16) {
            rk_scale_nv16(src, width, height, stride, (uint16_t *)source->display_rgb, RK_DISPLAY_W,
                          RK_DISPLAY_H, k);
        } else {
            rk_scale_yuyv(src, width, height, stride, (uint16_t *)source->display_rgb, RK_DISPLAY_W,
                          RK_DISPLAY_H, k);
        }
    } else {
        pthread_mutex_unlock(&source->mutex);
        return;
    }
    source->display_w = RK_DISPLAY_W;
    source->display_h = RK_DISPLAY_H;
    source->display_pts = pts_ns;
    source->display_seq++;
    source->display_publish_ns = now;
    source->display_rga = used_rga;
    pthread_cond_broadcast(&source->display_cv);
    pthread_mutex_unlock(&source->mutex);
}

void rk_source_release_display(void *opaque)
{
    struct rk_source *source = opaque;

    if (source == NULL) {
        return;
    }
    pthread_mutex_lock(&source->mutex);
    rk_rga_scale_release();
    /* The next capture may follow a mode change: read the matrix again. */
    source->display_csc_ns = 0;
    pthread_mutex_unlock(&source->mutex);
}

int32_t rk_source_display(void *opaque, const struct onekvm_video_display_request_v1 *request,
                          uint8_t *data, uint64_t capacity,
                          struct onekvm_video_display_result_v1 *result, char *error,
                          uint32_t error_capacity)
{
    struct rk_source *source = opaque;
    uint32_t timeout_ms;
    size_t need;
    struct timespec until;

    if (source == NULL || request == NULL || result == NULL || data == NULL ||
        request->struct_size < sizeof(*request) || result->struct_size < sizeof(*result)) {
        onekvm_set_error(error, error_capacity, "invalid display request");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    if (request->flags != 0 ||
        (request->pixel_format != 0 && request->pixel_format != ONEKVM_VIDEO_PIXEL_RGB565)) {
        onekvm_set_error(error, error_capacity, "unsupported display request");
        return ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
    }
    if (request->width < 0 || request->height < 0 ||
        (request->width == 0) != (request->height == 0) ||
        (request->width != 0 && ((request->width & 1) != 0 || (request->height & 1) != 0))) {
        onekvm_set_error(error, error_capacity, "invalid display size");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    timeout_ms = request->timeout_ms == 0 ? 250 : request->timeout_ms;
    if (timeout_ms > 1000) {
        timeout_ms = 1000;
    }
    pthread_mutex_lock(&source->mutex);
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += (time_t)(timeout_ms / 1000);
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    while (source->display_seq == 0 || source->display_seq == source->display_read_seq) {
        if (pthread_cond_timedwait(&source->display_cv, &source->mutex, &until) != 0) {
            if (source->display_seq == 0 || source->display_rgb == NULL) {
                pthread_mutex_unlock(&source->mutex);
                onekvm_set_error(error, error_capacity, "display timed out");
                return ONEKVM_VIDEO_RESOURCE_TIMEOUT;
            }
            break;
        }
    }
    if (source->display_rgb == NULL || source->display_w <= 0 || source->display_h <= 0) {
        pthread_mutex_unlock(&source->mutex);
        onekvm_set_error(error, error_capacity, "display is empty");
        return ONEKVM_VIDEO_RESOURCE_UNINITIALIZED;
    }
    need = (size_t)source->display_w * (size_t)source->display_h * 2u;
    if (capacity < need) {
        pthread_mutex_unlock(&source->mutex);
        onekvm_set_error(error, error_capacity, "display buffer is too small");
        return ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL;
    }
    memcpy(data, source->display_rgb, need);
    source->display_read_seq = source->display_seq;
    result->data_size = need;
    result->width = source->display_w;
    result->height = source->display_h;
    result->pixel_format = ONEKVM_VIDEO_PIXEL_RGB565;
    result->pts_ns = source->display_pts;
    result->flags = source->display_rga ? ONEKVM_VIDEO_DISPLAY_FLAG_RGA : 0;
    pthread_mutex_unlock(&source->mutex);
    return ONEKVM_VIDEO_RESOURCE_OK;
}

int32_t rk_source_input_format(void *opaque, struct onekvm_video_format_v1 *format)
{
    struct rk_source *source = opaque;

    int32_t width;
    int32_t height;
    int32_t fps;
    bool signal;

    if (source == NULL || format == NULL || format->struct_size < ONEKVM_VIDEO_FORMAT_V1_BASE_SIZE) {
        return -1;
    }
    /* Core asks for this on every encoded packet sent to WebSocket clients. */
    if (!cached_timings(source, &width, &height, &fps, &signal)) {
        pthread_mutex_lock(&source->mutex);
        query_timings_locked(source);
        width = source->width;
        height = source->height;
        fps = source->fps;
        signal = source->signal;
        pthread_mutex_unlock(&source->mutex);
    }
    format->width = width;
    format->height = height;
    format->fps = fps;
    format->pixel_format = ONEKVM_VIDEO_PIXEL_UNKNOWN;
    if (format->struct_size >= sizeof(*format)) {
        format->flags = signal ? 0 : ONEKVM_VIDEO_FORMAT_OUT_OF_RANGE;
    }
    return 0;
}

int rk_source_subdev_fd(void *opaque)
{
    struct rk_source *source = opaque;
    return source == NULL ? -1 : source->subdev_fd;
}

void rk_source_timings(void *opaque, int32_t *width, int32_t *height, int32_t *fps, bool *signal)
{
    struct rk_source *source = opaque;

    if (source == NULL) {
        return;
    }
    pthread_mutex_lock(&source->mutex);
    query_timings_locked(source);
    if (width != NULL) {
        *width = source->width;
    }
    if (height != NULL) {
        *height = source->height;
    }
    if (fps != NULL) {
        *fps = source->fps;
    }
    if (signal != NULL) {
        *signal = source->signal;
    }
    pthread_mutex_unlock(&source->mutex);
}

const char *rk_source_device(void *opaque)
{
    struct rk_source *source = opaque;
    return source == NULL ? ONEKVM_RK_MPP_DEFAULT_DEVICE : source->device;
}

static int open_edid_subdev(void)
{
    char path[64];
    return open_subdev_for_timings(path, sizeof(path));
}

/* PicoKVM boards carry either a TC358743 or an RK628F; tell them apart by the
 * name of the subdev that answers QUERY_DV_TIMINGS. */
static const char *bridge_chip_id(void)
{
    static const char *chip;
    char path[64];
    char sysfs[128];
    char name[64] = {0};
    const char *base;
    FILE *file;
    int fd;

    if (chip != NULL) {
        return chip;
    }
    fd = open_subdev_for_timings(path, sizeof(path));
    if (fd < 0) {
        return "tc358743";
    }
    close(fd);
    base = strrchr(path, '/');
    base = base == NULL ? path : base + 1;
    if (snprintf(sysfs, sizeof(sysfs), "/sys/class/video4linux/%s/name", base) >=
        (int)sizeof(sysfs)) {
        return "tc358743";
    }
    file = fopen(sysfs, "re");
    if (file == NULL) {
        return "tc358743";
    }
    if (fgets(name, sizeof(name), file) == NULL) {
        name[0] = '\0';
    }
    fclose(file);
    chip = strstr(name, "rk628") != NULL ? "rk628" : "tc358743";
    return chip;
}

int32_t rk_edid_capabilities(struct onekvm_video_edid_caps_v1 *caps)
{
    if (caps == NULL || caps->struct_size < sizeof(*caps)) {
        return -1;
    }
    caps->flags = ONEKVM_VIDEO_EDID_CAP_WRITABLE | ONEKVM_VIDEO_EDID_CAP_PERSISTENT |
                  ONEKVM_VIDEO_EDID_CAP_TEMPORARY | ONEKVM_VIDEO_EDID_CAP_HOTPLUG;
    caps->min_bytes = 128;
    caps->max_bytes = 256;
    caps->apply_policy = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
    caps->chip_id = bridge_chip_id();
    caps->board_id = "picokvm";
    return 0;
}

int32_t rk_edid_get(struct onekvm_video_edid_blob_v1 *edid, char *error, uint32_t error_capacity)
{
    int fd;
    struct v4l2_edid query;

    if (edid == NULL || edid->struct_size < sizeof(*edid)) {
        onekvm_set_error(error, error_capacity, "invalid EDID buffer");
        return -1;
    }
    fd = open_edid_subdev();
    if (fd < 0) {
        onekvm_set_error(error, error_capacity, "HDMI subdev unavailable");
        return -1;
    }
    memset(&query, 0, sizeof(query));
    query.pad = 0;
    query.blocks = 2;
    query.edid = edid->data;
    if (xioctl(fd, VIDIOC_G_EDID, &query) < 0) {
        onekvm_set_error(error, error_capacity, "VIDIOC_G_EDID: %s", strerror(errno));
        close(fd);
        return -1;
    }
    edid->size = query.blocks * 128u;
    close(fd);
    return 0;
}

int32_t rk_edid_set_mode(const struct onekvm_video_edid_blob_v1 *edid, uint32_t mode,
                         struct onekvm_video_edid_apply_result_v1 *apply, char *error,
                         uint32_t error_capacity)
{
    const struct rk_edid_paths *paths = &rk_edid_default_paths;
    int fd;

    if (edid == NULL || apply == NULL || edid->struct_size < sizeof(*edid) ||
        apply->struct_size < sizeof(*apply) ||
        (edid->size != 128 && edid->size != 256) ||
        (mode != ONEKVM_VIDEO_EDID_WRITE_PERMANENT && mode != ONEKVM_VIDEO_EDID_WRITE_TEMPORARY)) {
        onekvm_set_error(error, error_capacity, "invalid EDID write");
        return -1;
    }
    fd = open_edid_subdev();
    if (fd < 0) {
        onekvm_set_error(error, error_capacity, "HDMI subdev unavailable");
        return -1;
    }
    if (write_bridge_edid(fd, edid->data, edid->size) != 0) {
        onekvm_set_error(error, error_capacity, "VIDIOC_S_EDID: %s", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    /* The bridge now holds what this boot should use. */
    rk_edid_mark_restored(paths);
    apply->apply_required = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
    if (mode == ONEKVM_VIDEO_EDID_WRITE_PERMANENT &&
        rk_edid_remember(paths, edid->data, edid->size) != 0) {
        onekvm_set_error(error, error_capacity,
                         "EDID applied, but not saved for the next start: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int32_t rk_edid_set(const struct onekvm_video_edid_blob_v1 *edid,
                    struct onekvm_video_edid_apply_result_v1 *apply, char *error,
                    uint32_t error_capacity)
{
    return rk_edid_set_mode(edid, ONEKVM_VIDEO_EDID_WRITE_PERMANENT, apply, error,
                            error_capacity);
}
