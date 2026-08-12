/* SPDX-FileCopyrightText: 2026 ShinkoNet */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pbl/services/app_blob.h"

//! @addtogroup Foundation
//! @{
//!   @addtogroup Storage
//!   @{

//! Returns information about the current app's committed large blob.
int32_t app_blob_get_info(AppBlobInfo *info_out);

//! Returns currently available shared filesystem space in bytes.
size_t app_blob_get_free_size(void);

//! Replaces any existing blob with an uncommitted blob of the requested size.
int32_t app_blob_begin(uint32_t size);

//! Writes up to 1024 bytes into an uncommitted blob.
int app_blob_write(uint32_t offset, const void *data, size_t size);

//! Verifies the payload and makes it available to app_blob_read().
int32_t app_blob_commit(uint32_t expected_crc32);

//! Reads bytes from the current app's committed blob.
int app_blob_read(uint32_t offset, void *data, size_t size);

//! Deletes the current app's blob.
int32_t app_blob_delete(void);

//!   @}
//! @}
