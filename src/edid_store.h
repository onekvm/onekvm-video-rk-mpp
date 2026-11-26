/* SPDX-License-Identifier: MIT */
#ifndef ONEKVM_RK_EDID_STORE_H
#define ONEKVM_RK_EDID_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RK_EDID_MAX_BYTES 256u

/*
 * The HDMI bridges keep their EDID in RAM. A permanent write keeps a copy in
 * saved; the first source of each boot loads it, or the factory EDID when
 * there is none. restored lives on tmpfs, so a Core restart keeps whatever
 * the bridge holds, including a temporary write.
 */
struct rk_edid_paths {
    const char *factory;
    const char *saved_dir;
    const char *saved;
    const char *restored;
};

extern const struct rk_edid_paths rk_edid_default_paths;

/* Size of the EDID read from path; 0 unless it holds one or two whole blocks. */
size_t rk_edid_read(const char *path, uint8_t *edid, size_t capacity);
/* The EDID for this boot: the saved one, otherwise the factory one. */
size_t rk_edid_boot_choice(const struct rk_edid_paths *paths, uint8_t *edid, size_t capacity);
/* Keep a permanent write for the next boot. Writing the factory EDID drops the
 * copy, so a newer factory EDID applies after an update. */
int rk_edid_remember(const struct rk_edid_paths *paths, const uint8_t *edid, size_t size);
bool rk_edid_restored(const struct rk_edid_paths *paths);
void rk_edid_mark_restored(const struct rk_edid_paths *paths);
/* The RK628 driver's built-in EDIDs, which the bridge holds after probe. */
bool rk_edid_is_bridge_default(const uint8_t *edid, size_t size);
/* Whether the bridge needs the boot EDID: it has none, it still holds a
 * driver default, or this is the first source since boot and it differs. */
bool rk_edid_needs_write(const uint8_t *current, size_t current_size, const uint8_t *wanted,
                         size_t wanted_size, bool restored);

#endif
