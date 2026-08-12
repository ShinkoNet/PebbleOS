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

//! Returns the maximum blob payload that can currently be allocated while
//! retaining filesystem headroom.
size_t app_blob_get_free_size(void);

//! Starts a replacement of the existing blob with the requested payload size.
//! The committed blob remains readable until app_blob_commit() succeeds. An
//! unfinished transaction is discarded when its app or worker exits.
int32_t app_blob_begin(uint32_t size);

//! Writes up to 1024 bytes within the size passed to app_blob_begin(). Each
//! payload byte should be written once because the backing storage is NOR
//! flash.
int app_blob_write(uint32_t offset, const void *data, size_t size);

//! Verifies the whole payload against expected_crc32 and publishes it to
//! app_blob_read().
int32_t app_blob_commit(uint32_t expected_crc32);

//! Reads bytes from the current app's committed blob.
int app_blob_read(uint32_t offset, void *data, size_t size);

//! Deletes the current app's blob.
int32_t app_blob_delete(void);

//!   @}
//! @}
