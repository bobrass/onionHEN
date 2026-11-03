/* Copyright (C) 2025 OnionHEN / LightningMods

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include "common_utils.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <freebsd-helper.h>
#include <libgen.h>
#include <ps5/klog.h>
#include <onion/platform.h>
#include <onion/proc_query.h>
#include <onion/payload.h>

typedef struct app_info {
  uint32_t app_id;
  uint64_t unknown1;
  uint32_t app_type;
  char     title_id[10];
  char     unknown2[0x3c];
} app_info_t;

int launchApp(const char *titleId);
int sceSystemServiceGetAppId(const char *title_id);
void free(void *ptr);
void *malloc(size_t size);
int elfldr_set_procname(pid_t pid, const char* name);

int sceKernelGetProcessName(int pid, char *name);
int sceKernelGetAppInfo(int pid, app_info_t *title);

/* OnionHEN: user payloads launch exclusively via private elfldr :9020.
 * ptrace attach/mmap: libonion_elfldr (pt_attach / pt_mmap). Authid is raised
 * once in util main via set_ucred_to_ptrace(), not flipped per ptrace call.
 */

/*
 * Thin wrapper over the shared helper; returns 0 when the console has a
 * usable IPv4 address, -1 otherwise.
 */
int get_ip_address(char *ip_address, size_t size)
{
	const onion_net_ip_status status = onion_net_get_ip_address(ip_address, size);
	return status == ONION_NET_IP_OK ? 0 : -1;
}





bool copyFile(const char *source, const char *destination)
{
    FILE *src = NULL;
    FILE *dest = NULL;
    struct stat source_stat;
    struct stat destination_stat;
    char buffer[8192];
    char temp_path[1024];
    size_t bytes;
    mode_t destination_mode = 0666;
    bool destination_exists = false;
    int temp_fd = -1;
    int copy_error = 0;

    if (source == NULL || source[0] == '\0' || destination == NULL ||
        destination[0] == '\0') {
        errno = EINVAL;
        onion_notify(false, "notify.fs.copy_file", "");
        return false;
    }
    src = fopen(source, "rb");
    if (src == NULL) {
        onion_notify(false, "notify.fs.copy_file", source);
        LOG_ERROR("copyFile failed for %s: %s", source, strerror(errno));
        return false;
    }
    if (fstat(fileno(src), &source_stat) != 0) {
        const int saved_errno = errno;
        fclose(src);
        errno = saved_errno;
        onion_notify(false, "notify.fs.copy_file", source);
        LOG_ERROR("copyFile could not stat source %s: %s", source,
                  strerror(errno));
        return false;
    }
    if (lstat(destination, &destination_stat) == 0) {
        if (!S_ISREG(destination_stat.st_mode) ||
            (source_stat.st_dev == destination_stat.st_dev &&
             source_stat.st_ino == destination_stat.st_ino)) {
            fclose(src);
            errno = EINVAL;
            onion_notify(false, "notify.fs.copy_file", destination);
            LOG_ERROR("copyFile rejected destination %s", destination);
            return false;
        }
        destination_exists = true;
        destination_mode = destination_stat.st_mode & 0777;
    } else if (errno != ENOENT) {
        const int saved_errno = errno;
        fclose(src);
        errno = saved_errno;
        onion_notify(false, "notify.fs.copy_file", destination);
        LOG_ERROR("copyFile could not inspect destination %s: %s",
                  destination, strerror(errno));
        return false;
    }

    for (unsigned int attempt = 0; attempt < 1024; ++attempt) {
        const int written = snprintf(temp_path, sizeof(temp_path),
                                     "%s.tmp.%d.%u", destination,
                                     (int)getpid(), attempt);
        if (written < 0 || (size_t)written >= sizeof(temp_path)) {
            fclose(src);
            errno = ENAMETOOLONG;
            onion_notify(false, "notify.fs.copy_file", destination);
            return false;
        }
        temp_fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL,
                       destination_mode);
        if (temp_fd >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (temp_fd < 0) {
        const int saved_errno = errno;
        fclose(src);
        errno = saved_errno;
        onion_notify(false, "notify.fs.copy_file", destination);
        LOG_ERROR("copyFile could not create staging file for %s: %s",
                  destination, strerror(errno));
        return false;
    }
    if (destination_exists && fchmod(temp_fd, destination_mode) != 0) {
        const int saved_errno = errno;
        close(temp_fd);
        unlink(temp_path);
        fclose(src);
        errno = saved_errno;
        onion_notify(false, "notify.fs.copy_file", destination);
        return false;
    }
    dest = fdopen(temp_fd, "wb");
    if (dest == NULL) {
        const int saved_errno = errno;
        close(temp_fd);
        unlink(temp_path);
        fclose(src);
        errno = saved_errno;
        onion_notify(false, "notify.fs.copy_file", destination);
        return false;
    }

    errno = 0;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, dest) != bytes) {
            copy_error = errno != 0 ? errno : EIO;
            break;
        }
    }
    if (copy_error == 0 && ferror(src)) {
        copy_error = errno != 0 ? errno : EIO;
    }
    if (fclose(src) != 0 && copy_error == 0) {
        copy_error = errno != 0 ? errno : EIO;
    }
    if (fflush(dest) != 0 && copy_error == 0) {
        copy_error = errno != 0 ? errno : EIO;
    }
    if (copy_error == 0 && fsync(fileno(dest)) != 0) {
        copy_error = errno != 0 ? errno : EIO;
    }
    if (fclose(dest) != 0 && copy_error == 0) {
        copy_error = errno != 0 ? errno : EIO;
    }
    if (copy_error != 0) {
        unlink(temp_path);
        errno = copy_error;
        onion_notify(false, "notify.fs.copy_file", destination);
        LOG_ERROR("copyFile failed while copying %s to %s: %s", source,
                  destination, strerror(errno));
        return false;
    }
    if (rename(temp_path, destination) != 0) {
        const int saved_errno = errno;
        unlink(temp_path);
        errno = saved_errno;
        onion_notify(false, "notify.fs.copy_file", destination);
        LOG_ERROR("copyFile could not publish %s: %s", destination,
                  strerror(errno));
        return false;
    }
    return true;
}



bool load_payload(const char *path) {
  /* Payload .elf only — .plugin packages removed. */
  return onion_payload_load(path, /*filename=*/NULL);
}

bool load_payload_with_key(const char *path, const char *key) {
  return onion_payload_load_with_key(path, /*filename=*/NULL, key);
}

int launchApp(const char *titleId)
{
	int id = 0;

	uint32_t res = sceUserServiceGetForegroundUser(&id);
	if (res != 0)
	{
		LOG_ERROR("sceUserServiceGetForegroundUser failed: 0x%x", res);
		return res;
	}
	LOG_DEBUG("[LA] user id %u", id);

    LncAppParam param;
	param.sz = sizeof(LncAppParam);
	param.user_id = id;
	param.app_opt = 0;
	param.crash_report = 0;
	param.check_flag = Flag_None;


	LOG_DEBUG("calling sceLncUtilLaunchApp");
	int err = sceLncUtilLaunchApp(titleId, NULL, &param);
	LOG_DEBUG("sceLncUtilLaunchApp returned 0x%x", (uint32_t)err);
	if (err >= 0)
	{
		return err;
	}
	switch ((uint32_t)err)
	{
	case SCE_LNC_UTIL_ERROR_ALREADY_RUNNING:
		LOG_WARN("app %s is already running", titleId);
		break;
	case SCE_LNC_ERROR_APP_NOT_FOUND:
		LOG_ERROR("app %s not found", titleId);
		onion_notify(true, "notify.app.not_found", titleId);
		break;
	default:
		LOG_ERROR("[LA] unknown error 0x%x", (uint32_t)err);
		// onion_notify(true, "unknown error 0x%llx", (uint32_t)err);
		break;
	}
	return err;
}
