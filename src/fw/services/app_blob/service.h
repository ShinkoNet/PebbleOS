/* SPDX-FileCopyrightText: 2026 ShinkoNet */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/pebble_tasks.h"
#include "pbl/services/app_blob.h"
#include "pbl/util/uuid.h"
#include "system/status_codes.h"

void app_blob_service_init(void);
status_t app_blob_service_get_info(const Uuid *uuid, AppBlobInfo *info_out);
size_t app_blob_service_get_free_size(void);
status_t app_blob_service_begin(const Uuid *uuid, PebbleTask owner, uint32_t size);
int app_blob_service_write(const Uuid *uuid, PebbleTask owner, uint32_t offset, const void *data,
                           size_t size);
status_t app_blob_service_commit(const Uuid *uuid, PebbleTask owner, uint32_t expected_crc32);
int app_blob_service_read(const Uuid *uuid, uint32_t offset, void *data, size_t size);
status_t app_blob_service_delete_for_task(const Uuid *uuid, PebbleTask owner);
status_t app_blob_service_delete(const Uuid *uuid);
void app_blob_service_process_cleanup(const Uuid *uuid, PebbleTask owner);

#if UNITTEST
int app_blob_service_test_get_read_fd(void);
#endif
