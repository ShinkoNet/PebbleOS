/* SPDX-FileCopyrightText: 2026 ShinkoNet */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pbl/util/uuid.h"
#include "system/status_codes.h"

//! Maximum payload stored by the app blob service.
#define APP_BLOB_MAX_SIZE (8u * 1024u * 1024u)

typedef struct AppBlobInfo {
  uint32_t size;
  uint32_t crc32;
} AppBlobInfo;

status_t app_blob_service_get_info(const Uuid *uuid, AppBlobInfo *info_out);
size_t app_blob_service_get_free_size(void);
status_t app_blob_service_begin(const Uuid *uuid, uint32_t size);
int app_blob_service_write(const Uuid *uuid, uint32_t offset, const void *data, size_t size);
status_t app_blob_service_commit(const Uuid *uuid, uint32_t expected_crc32);
int app_blob_service_read(const Uuid *uuid, uint32_t offset, void *data, size_t size);
status_t app_blob_service_delete(const Uuid *uuid);
