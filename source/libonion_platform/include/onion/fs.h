/* Copyright (C) 2025 OnionHEN / LightningMods
 *
 * Shared filesystem helpers (if_exists / touch / mkdir_tree / rmtree /
 * write_file_atomic).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True if path exists (stat succeeds). */
bool if_exists(const char *path);

/**
 * Create or truncate a regular file at path (mode 0777); refuses symlinks.
 */
bool touch_file(const char *path);

/**
 * Write @data to @path via a sibling temp file, fsync, then rename.
 * Failure leaves any previous @path untouched. @data may be NULL when
 * @size is 0.
 */
bool write_file_atomic(const char *path, const void *data, size_t size);

/** Create path and any missing parents (mode 0777). True if the directory exists. */
bool mkdir_tree(const char *path);

/**
 * Recursively delete directory tree at path. Refuses filesystem-root and
 * dot-component targets ("." or "..").
 */
bool rmtree(const char *path);

typedef void (*onion_fs_progress_fn)(size_t completed, size_t total,
                                     void *user);

/**
 * Recursively delete a tree and report completed entries, including dirs.
 * Applies the same path guards as rmtree().
 */
bool rmtree_with_progress(const char *path, onion_fs_progress_fn progress,
                          void *user);

#ifdef __cplusplus
}
#endif
