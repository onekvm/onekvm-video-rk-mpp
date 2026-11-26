/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "edid_store.h"

static char root[64];
static char factory_path[PATH_MAX];
static char saved_dir[PATH_MAX];
static char saved_path[PATH_MAX];
static char restored_path[PATH_MAX];
static struct rk_edid_paths paths;

static void fill(uint8_t *edid, size_t size, uint8_t product)
{
    memset(edid, 0, size);
    memcpy(edid, "\x00\xff\xff\xff\xff\xff\xff\x00", 8);
    edid[8] = 0x3d;
    edid[9] = 0x76;
    edid[10] = product;
}

static void write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

int main(void)
{
    uint8_t factory[256];
    uint8_t custom[256];
    uint8_t other[128];
    uint8_t buffer[256];
    uint8_t rk_default[256];

    strcpy(root, "/tmp/edid-store-test.XXXXXX");
    assert(mkdtemp(root) != NULL);
    snprintf(factory_path, sizeof(factory_path), "%s/factory.edid.bin", root);
    snprintf(saved_dir, sizeof(saved_dir), "%s/state", root);
    snprintf(saved_path, sizeof(saved_path), "%s/state/edid.bin", root);
    snprintf(restored_path, sizeof(restored_path), "%s/restored", root);
    paths = (struct rk_edid_paths){factory_path, saved_dir, saved_path, restored_path};

    fill(factory, sizeof(factory), 0x0c);
    fill(custom, sizeof(custom), 0x42);
    fill(other, sizeof(other), 0x43);
    fill(rk_default, sizeof(rk_default), 0);
    memcpy(rk_default + 8, "\x49\x73\x8d\x62", 4);

    /* No factory file: nothing to load. */
    assert(rk_edid_boot_choice(&paths, buffer, sizeof(buffer)) == 0);
    assert(!rk_edid_needs_write(NULL, 0, buffer, 0, false));

    /* Only whole one- or two-block files count. */
    write_file(factory_path, factory, 200);
    assert(rk_edid_read(factory_path, buffer, sizeof(buffer)) == 0);
    write_file(factory_path, factory, sizeof(factory));
    assert(rk_edid_boot_choice(&paths, buffer, sizeof(buffer)) == 256);
    assert(memcmp(buffer, factory, 256) == 0);

    /* Boot: a bridge with no EDID or a driver default gets the boot EDID. */
    assert(rk_edid_needs_write(NULL, 0, factory, 256, false));
    assert(rk_edid_needs_write(rk_default, 256, factory, 256, true));
    /* The RK628 driver already loads the factory EDID: no replug. */
    assert(!rk_edid_needs_write(factory, 256, factory, 256, false));

    /* A permanent write is kept and wins at the next boot. */
    assert(rk_edid_remember(&paths, custom, sizeof(custom)) == 0);
    assert(access(saved_path, F_OK) == 0);
    assert(rk_edid_boot_choice(&paths, buffer, sizeof(buffer)) == 256);
    assert(memcmp(buffer, custom, 256) == 0);
    assert(rk_edid_needs_write(factory, 256, custom, 256, false));
    assert(!rk_edid_needs_write(custom, 256, custom, 256, false));

    /* After the first source of the boot, a temporary EDID stays. */
    assert(!rk_edid_restored(&paths));
    rk_edid_mark_restored(&paths);
    assert(rk_edid_restored(&paths));
    assert(!rk_edid_needs_write(other, 128, custom, 256, true));
    assert(rk_edid_needs_write(other, 128, custom, 256, false));

    /* A one-block permanent write replaces the copy. */
    assert(rk_edid_remember(&paths, other, sizeof(other)) == 0);
    assert(rk_edid_boot_choice(&paths, buffer, sizeof(buffer)) == 128);
    assert(memcmp(buffer, other, 128) == 0);

    /* Writing the factory EDID drops the copy, twice in a row too. */
    assert(rk_edid_remember(&paths, factory, sizeof(factory)) == 0);
    assert(access(saved_path, F_OK) != 0);
    assert(rk_edid_remember(&paths, factory, sizeof(factory)) == 0);
    assert(rk_edid_boot_choice(&paths, buffer, sizeof(buffer)) == 256);
    assert(memcmp(buffer, factory, 256) == 0);

    assert(rk_edid_remember(&paths, custom, 100) != 0);

    unlink(restored_path);
    unlink(factory_path);
    rmdir(saved_dir);
    rmdir(root);
    puts("edid-store: ok");
    return 0;
}
