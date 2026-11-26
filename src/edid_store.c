/* SPDX-License-Identifier: MIT */

#include "edid_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const struct rk_edid_paths rk_edid_default_paths = {
    /* The same file on every OneKVM machine; the machine package installs it. */
    .factory = "/usr/share/onekvm/edid/presets/factory.edid.bin",
    .saved_dir = "/var/lib/onekvm/video-rk-mpp",
    .saved = "/var/lib/onekvm/video-rk-mpp/edid.bin",
    .restored = "/run/onekvm/rk-mpp-edid-restored",
};

static bool edid_size_valid(size_t size)
{
    return size == 128 || size == 256;
}

size_t rk_edid_read(const char *path, uint8_t *edid, size_t capacity)
{
    uint8_t extra;
    ssize_t n;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    n = read(fd, edid, capacity);
    /* A longer file is not an EDID this bridge can take. */
    if (n > 0 && read(fd, &extra, 1) != 0) {
        n = -1;
    }
    close(fd);
    if (n < 0 || !edid_size_valid((size_t)n)) {
        return 0;
    }
    return (size_t)n;
}

size_t rk_edid_boot_choice(const struct rk_edid_paths *paths, uint8_t *edid, size_t capacity)
{
    size_t size = rk_edid_read(paths->saved, edid, capacity);

    return size != 0 ? size : rk_edid_read(paths->factory, edid, capacity);
}

static int sync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int result;

    if (fd < 0) {
        return -1;
    }
    result = fsync(fd);
    close(fd);
    return result;
}

static int write_all(int fd, const uint8_t *data, size_t size)
{
    while (size > 0) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        data += n;
        size -= (size_t)n;
    }
    return 0;
}

int rk_edid_remember(const struct rk_edid_paths *paths, const uint8_t *edid, size_t size)
{
    uint8_t factory[RK_EDID_MAX_BYTES];
    char temp[256];
    size_t factory_size;
    int fd;

    if (!edid_size_valid(size)) {
        errno = EINVAL;
        return -1;
    }
    factory_size = rk_edid_read(paths->factory, factory, sizeof(factory));
    if (factory_size == size && memcmp(factory, edid, size) == 0) {
        if (unlink(paths->saved) != 0) {
            return errno == ENOENT ? 0 : -1;
        }
        return sync_dir(paths->saved_dir);
    }
    if (mkdir(paths->saved_dir, 0750) != 0 && errno != EEXIST) {
        return -1;
    }
    if (snprintf(temp, sizeof(temp), "%s.new", paths->saved) >= (int)sizeof(temp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
    if (fd < 0) {
        return -1;
    }
    if (write_all(fd, edid, size) != 0 || fsync(fd) != 0) {
        int saved_errno = errno;
        close(fd);
        unlink(temp);
        errno = saved_errno;
        return -1;
    }
    close(fd);
    if (rename(temp, paths->saved) != 0) {
        int saved_errno = errno;
        unlink(temp);
        errno = saved_errno;
        return -1;
    }
    return sync_dir(paths->saved_dir);
}

bool rk_edid_restored(const struct rk_edid_paths *paths)
{
    return access(paths->restored, F_OK) == 0;
}

void rk_edid_mark_restored(const struct rk_edid_paths *paths)
{
    int fd = open(paths->restored, O_WRONLY | O_CREAT | O_CLOEXEC, 0640);

    if (fd >= 0) {
        close(fd);
    }
}

bool rk_edid_is_bridge_default(const uint8_t *edid, size_t size)
{
    /* Vendor and product IDs of the EDIDs older RK628 drivers load at probe. */
    static const uint8_t kIds[][4] = {
        {0x49, 0x73, 0x8d, 0x62},
        {0x24, 0xd0, 0x8f, 0x62},
    };
    size_t i;

    if (size < 128) {
        return false;
    }
    for (i = 0; i < sizeof(kIds) / sizeof(kIds[0]); i++) {
        if (memcmp(edid + 8, kIds[i], sizeof(kIds[i])) == 0) {
            return true;
        }
    }
    return false;
}

bool rk_edid_needs_write(const uint8_t *current, size_t current_size, const uint8_t *wanted,
                         size_t wanted_size, bool restored)
{
    if (wanted_size == 0) {
        return false;
    }
    if (current_size == 0 || rk_edid_is_bridge_default(current, current_size)) {
        return true;
    }
    if (restored) {
        return false;
    }
    return current_size != wanted_size || memcmp(current, wanted, wanted_size) != 0;
}
