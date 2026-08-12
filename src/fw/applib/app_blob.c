/* SPDX-FileCopyrightText: 2026 ShinkoNet */
/* SPDX-License-Identifier: Apache-2.0 */

#include "app_blob.h"

#include "kernel/pebble_tasks.h"
#include "process_management/process_manager.h"
#include "services/app_blob/service.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"

static const Uuid *prv_current_uuid(void) {
  return &sys_process_manager_get_current_process_md()->uuid;
}

DEFINE_SYSCALL(status_t, app_blob_get_info, AppBlobInfo *info_out) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(info_out, sizeof(*info_out));
  }
  return app_blob_service_get_info(prv_current_uuid(), info_out);
}

DEFINE_SYSCALL(size_t, app_blob_get_free_size, void) {
  return app_blob_service_get_free_size();
}

DEFINE_SYSCALL(status_t, app_blob_begin, uint32_t size) {
  return app_blob_service_begin(prv_current_uuid(), pebble_task_get_current(), size);
}

DEFINE_SYSCALL(int, app_blob_write, uint32_t offset, const void *data, size_t size) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(data, size);
  }
  return app_blob_service_write(prv_current_uuid(), pebble_task_get_current(), offset, data, size);
}

DEFINE_SYSCALL(status_t, app_blob_commit, uint32_t expected_crc32) {
  return app_blob_service_commit(prv_current_uuid(), pebble_task_get_current(), expected_crc32);
}

DEFINE_SYSCALL(int, app_blob_read, uint32_t offset, void *data, size_t size) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(data, size);
  }
  return app_blob_service_read(prv_current_uuid(), offset, data, size);
}

DEFINE_SYSCALL(status_t, app_blob_delete, void) {
  return app_blob_service_delete_for_task(prv_current_uuid(), pebble_task_get_current());
}
