/* audio_oss.h — OSS /dev/dsp and /dev/mixer via open-file ops
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef HL_AUDIO_OSS_H
#define HL_AUDIO_OSS_H

#include <stdint.h>
#include <stddef.h>

void hl_audio_oss_register_devices(void);
void hl_audio_oss_init(void);

/* Fork policy: recreate-empty independent stream in child.
 * Used by fork_ipc to serialize/restore OSS open-file objects. */
struct hl_open_file;
struct hl_fork_object_record;

int hl_oss_fd_needs_recreate(int fd_type);
int hl_oss_fork_export(struct hl_open_file *of, struct hl_fork_object_record *out);
struct hl_open_file *hl_oss_fork_import(const struct hl_fork_object_record *in);

/* Neutral (mode,rdev) for an OSS fd, for statx AT_EMPTY_PATH. 0 = OSS. */
int hl_oss_fd_stat_meta(int guest_fd, uint32_t *mode_out, uint64_t *rdev_out);

#endif /* HL_AUDIO_OSS_H */
