/* SPDX-FileCopyrightText: 2026 ShinkoNet */
/* SPDX-License-Identifier: Apache-2.0 */

#include "services/app_blob/service.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pbl/drivers/task_watchdog.h"
#include "pbl/os/mutex.h"
#include "pbl/services/filesystem/pfs.h"
#include "pbl/util/crc32.h"

#define APP_BLOB_FILE_NAME_MAX_LENGTH sizeof("ab000102030405060708090a0b0c0d0e0f")
#define APP_BLOB_MAGIC 0x4d4c4241u /* "ABLM" in little endian. */
#define APP_BLOB_VERSION 1u
#define APP_BLOB_PFS_RESERVE_SIZE (1024u * 1024u)
#define APP_BLOB_CRC_WATCHDOG_INTERVAL (16u * 1024u)

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t version;
  uint32_t size;
  uint32_t size_inverse;
  uint32_t crc32;
  uint32_t crc32_inverse;
} AppBlobHeader;

typedef struct {
  bool active;
  bool replacing;
  Uuid uuid;
  PebbleTask owner;
  int fd;
  uint32_t size;
} AppBlobTransaction;

static PebbleMutex *s_mutex;
static AppBlobTransaction s_transaction;

static status_t prv_get_file_name(char *name, size_t name_size, const Uuid *uuid) {
  if (!name || !uuid) {
    return E_INVALID_ARGUMENT;
  }

  const uint8_t *b = (const uint8_t *)uuid;
  int length = snprintf(name, name_size,
                        "ab%02x%02x%02x%02x%02x%02x%02x%02x"
                        "%02x%02x%02x%02x%02x%02x%02x%02x",
                        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
                        b[12], b[13], b[14], b[15]);
  return (length > 0 && (size_t)length < name_size) ? S_SUCCESS : E_RANGE;
}

static bool prv_header_valid(const AppBlobHeader *header, size_t file_size) {
  return header->magic == APP_BLOB_MAGIC && header->version == APP_BLOB_VERSION &&
         header->size <= APP_BLOB_MAX_SIZE && header->size_inverse == ~header->size &&
         header->crc32_inverse == ~header->crc32 && file_size == sizeof(*header) + header->size;
}

static status_t prv_open(const Uuid *uuid, uint8_t flags, int *fd_out) {
  char name[APP_BLOB_FILE_NAME_MAX_LENGTH];
  status_t status = prv_get_file_name(name, sizeof(name), uuid);
  if (FAILED(status)) {
    return status;
  }

  int fd = pfs_open(name, flags, FILE_TYPE_STATIC, 0);
  if (fd < 0) {
    return fd;
  }
  *fd_out = fd;
  return S_SUCCESS;
}

static status_t prv_read_header(int fd, AppBlobHeader *header_out) {
  const size_t file_size = pfs_get_file_size(fd);
  if (file_size < sizeof(*header_out)) {
    return E_DOES_NOT_EXIST;
  }
  if (pfs_seek(fd, 0, FSeekSet) < 0 ||
      pfs_read(fd, header_out, sizeof(*header_out)) != (int)sizeof(*header_out)) {
    return E_ERROR;
  }
  return prv_header_valid(header_out, file_size) ? S_SUCCESS : E_DOES_NOT_EXIST;
}

static size_t prv_get_free_size(void) {
  const size_t unavailable = APP_BLOB_PFS_RESERVE_SIZE + sizeof(AppBlobHeader);
  const size_t available = get_available_pfs_space();
  if (available <= unavailable) {
    return 0;
  }

  const size_t payload_size = available - unavailable;
  return payload_size < APP_BLOB_MAX_SIZE ? payload_size : APP_BLOB_MAX_SIZE;
}

static bool prv_transaction_owned_by(const Uuid *uuid, PebbleTask owner) {
  return s_transaction.active && s_transaction.owner == owner &&
         uuid_equal(&s_transaction.uuid, uuid);
}

static status_t prv_abort_transaction(void) {
  if (!s_transaction.active) {
    return S_NO_ACTION_REQUIRED;
  }

  status_t status = s_transaction.replacing ? pfs_abort_overwrite(s_transaction.fd)
                                            : pfs_close_and_remove(s_transaction.fd);
  s_transaction = (AppBlobTransaction){.fd = -1};
  return status;
}

void app_blob_service_init(void) {
  if (!s_mutex) {
    s_mutex = mutex_create();
  }
  s_transaction = (AppBlobTransaction){.fd = -1};
}

status_t app_blob_service_get_info(const Uuid *uuid, AppBlobInfo *info_out) {
  if (!uuid || !info_out) {
    return E_INVALID_ARGUMENT;
  }

  mutex_lock(s_mutex);
  int fd;
  status_t status = prv_open(uuid, OP_FLAG_READ, &fd);
  if (FAILED(status)) {
    goto cleanup;
  }

  AppBlobHeader header;
  status = prv_read_header(fd, &header);
  status_t close_status = pfs_close(fd);
  if (PASSED(status) && FAILED(close_status)) {
    status = close_status;
  }
  if (FAILED(status)) {
    goto cleanup;
  }

  *info_out = (AppBlobInfo){
      .size = header.size,
      .crc32 = header.crc32,
  };

cleanup:
  mutex_unlock(s_mutex);
  return status;
}

size_t app_blob_service_get_free_size(void) {
  mutex_lock(s_mutex);
  size_t size = prv_get_free_size();
  mutex_unlock(s_mutex);
  return size;
}

status_t app_blob_service_begin(const Uuid *uuid, PebbleTask owner, uint32_t size) {
  if (!uuid || size > APP_BLOB_MAX_SIZE) {
    return E_INVALID_ARGUMENT;
  }

  mutex_lock(s_mutex);
  status_t status = S_SUCCESS;
  if (s_transaction.active) {
    if (s_transaction.owner != owner) {
      status = E_BUSY;
      goto cleanup;
    }
    status = prv_abort_transaction();
    if (FAILED(status)) {
      goto cleanup;
    }
  }

  char name[APP_BLOB_FILE_NAME_MAX_LENGTH];
  status = prv_get_file_name(name, sizeof(name), uuid);
  if (FAILED(status)) {
    goto cleanup;
  }

  bool replacing = false;
  int old_fd = pfs_open(name, OP_FLAG_READ, FILE_TYPE_STATIC, 0);
  if (old_fd >= 0) {
    AppBlobHeader old_header;
    status = prv_read_header(old_fd, &old_header);
    status_t close_status = pfs_close(old_fd);
    if (PASSED(status) && FAILED(close_status)) {
      status = close_status;
    }

    if (PASSED(status)) {
      replacing = true;
    } else if (status == E_DOES_NOT_EXIST) {
      status = pfs_remove(name);
      if (FAILED(status)) {
        goto cleanup;
      }
    } else {
      goto cleanup;
    }
  } else if (old_fd != E_DOES_NOT_EXIST) {
    status = old_fd;
    goto cleanup;
  }

  if (size > prv_get_free_size()) {
    status = E_OUT_OF_STORAGE;
    goto cleanup;
  }

  const size_t file_size = sizeof(AppBlobHeader) + size;
  const uint8_t flags =
      replacing ? (OP_FLAG_READ | OP_FLAG_OVERWRITE) : (OP_FLAG_READ | OP_FLAG_WRITE);
  int fd = pfs_open(name, flags, FILE_TYPE_STATIC, file_size);
  if (fd < 0) {
    status = fd;
    goto cleanup;
  }

  s_transaction = (AppBlobTransaction){
      .active = true,
      .replacing = replacing,
      .uuid = *uuid,
      .owner = owner,
      .fd = fd,
      .size = size,
  };

cleanup:
  mutex_unlock(s_mutex);
  return status;
}

int app_blob_service_write(const Uuid *uuid, PebbleTask owner, uint32_t offset, const void *data,
                           size_t size) {
  if (!uuid || (!data && size != 0) || size > 1024u) {
    return E_INVALID_ARGUMENT;
  }

  mutex_lock(s_mutex);
  int result = E_INVALID_OPERATION;
  if (!s_transaction.active) {
    goto cleanup;
  }
  if (!prv_transaction_owned_by(uuid, owner)) {
    result = E_BUSY;
    goto cleanup;
  }
  if (offset > s_transaction.size || size > s_transaction.size - offset) {
    result = E_RANGE;
    goto cleanup;
  }
  if (size == 0) {
    result = 0;
    goto cleanup;
  }

  if (pfs_seek(s_transaction.fd, (int)(sizeof(AppBlobHeader) + offset), FSeekSet) < 0) {
    result = E_ERROR;
    goto cleanup;
  }
  int written = pfs_write(s_transaction.fd, data, size);
  result = written == (int)size ? written : E_ERROR;

cleanup:
  mutex_unlock(s_mutex);
  return result;
}

status_t app_blob_service_commit(const Uuid *uuid, PebbleTask owner, uint32_t expected_crc32) {
  if (!uuid) {
    return E_INVALID_ARGUMENT;
  }

  mutex_lock(s_mutex);
  status_t status = E_INVALID_OPERATION;
  if (!s_transaction.active) {
    goto cleanup;
  }
  if (!prv_transaction_owned_by(uuid, owner)) {
    status = E_BUSY;
    goto cleanup;
  }
  if (pfs_seek(s_transaction.fd, sizeof(AppBlobHeader), FSeekSet) < 0) {
    status = E_ERROR;
    goto cleanup;
  }

  uint32_t crc = CRC32_INIT;
  uint8_t buffer[256];
  size_t remaining = s_transaction.size;
  size_t bytes_since_watchdog = 0;
  while (remaining) {
    size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
    if (pfs_read(s_transaction.fd, buffer, chunk) != (int)chunk) {
      status = E_ERROR;
      goto cleanup;
    }
    crc = crc32(crc, buffer, chunk);
    remaining -= chunk;
    bytes_since_watchdog += chunk;
    if (bytes_since_watchdog >= APP_BLOB_CRC_WATCHDOG_INTERVAL) {
      task_watchdog_bit_set(pebble_task_get_current());
      bytes_since_watchdog = 0;
    }
  }

  if (crc != expected_crc32) {
    status = E_ERROR;
    goto cleanup;
  }

  const AppBlobHeader header = {
      .magic = APP_BLOB_MAGIC,
      .version = APP_BLOB_VERSION,
      .size = s_transaction.size,
      .size_inverse = ~s_transaction.size,
      .crc32 = crc,
      .crc32_inverse = ~crc,
  };
  if (pfs_seek(s_transaction.fd, 0, FSeekSet) < 0 ||
      pfs_write(s_transaction.fd, &header, sizeof(header)) != (int)sizeof(header)) {
    status = E_ERROR;
    goto cleanup;
  }

  status = pfs_close(s_transaction.fd);
  if (PASSED(status)) {
    s_transaction = (AppBlobTransaction){.fd = -1};
  }

cleanup:
  mutex_unlock(s_mutex);
  return status;
}

int app_blob_service_read(const Uuid *uuid, uint32_t offset, void *data, size_t size) {
  if (!uuid || (!data && size != 0)) {
    return E_INVALID_ARGUMENT;
  }

  mutex_lock(s_mutex);
  int fd;
  int result = prv_open(uuid, OP_FLAG_READ | OP_FLAG_USE_PAGE_CACHE, &fd);
  if (result < 0) {
    goto cleanup;
  }

  AppBlobHeader header;
  status_t status = prv_read_header(fd, &header);
  if (FAILED(status)) {
    result = status;
    goto close;
  }
  if (offset > header.size || size > header.size - offset) {
    result = E_RANGE;
    goto close;
  }
  if (size == 0) {
    result = 0;
    goto close;
  }

  if (pfs_seek(fd, (int)(sizeof(AppBlobHeader) + offset), FSeekSet) < 0) {
    result = E_ERROR;
    goto close;
  }
  result = pfs_read(fd, data, size);

close: {
  status_t close_status = pfs_close(fd);
  if (result >= 0 && FAILED(close_status)) {
    result = close_status;
  }
}
cleanup:
  mutex_unlock(s_mutex);
  return result;
}

static status_t prv_delete(const Uuid *uuid, bool force, PebbleTask owner) {
  if (!uuid) {
    return E_INVALID_ARGUMENT;
  }

  mutex_lock(s_mutex);
  status_t status = S_SUCCESS;
  bool aborted_transaction = false;
  if (s_transaction.active && uuid_equal(&s_transaction.uuid, uuid)) {
    if (!force && s_transaction.owner != owner) {
      status = E_BUSY;
      goto cleanup;
    }
    status = prv_abort_transaction();
    if (FAILED(status)) {
      goto cleanup;
    }
    aborted_transaction = true;
  }

  char name[APP_BLOB_FILE_NAME_MAX_LENGTH];
  status = prv_get_file_name(name, sizeof(name), uuid);
  if (PASSED(status)) {
    status = pfs_remove(name);
    if (aborted_transaction && status == E_DOES_NOT_EXIST) {
      status = S_SUCCESS;
    }
  }

cleanup:
  mutex_unlock(s_mutex);
  return status;
}

status_t app_blob_service_delete_for_task(const Uuid *uuid, PebbleTask owner) {
  return prv_delete(uuid, false, owner);
}

status_t app_blob_service_delete(const Uuid *uuid) {
  return prv_delete(uuid, true, PebbleTask_Unknown);
}

void app_blob_service_process_cleanup(const Uuid *uuid, PebbleTask owner) {
  if (!uuid) {
    return;
  }

  mutex_lock(s_mutex);
  if (prv_transaction_owned_by(uuid, owner)) {
    prv_abort_transaction();
  }
  mutex_unlock(s_mutex);
}
