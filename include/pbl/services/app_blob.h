/* SPDX-FileCopyrightText: 2026 ShinkoNet */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

//! Maximum payload stored by the app blob service.
#define APP_BLOB_MAX_SIZE (8u * 1024u * 1024u)

typedef struct AppBlobInfo {
  uint32_t size;
  uint32_t crc32;
} AppBlobInfo;
