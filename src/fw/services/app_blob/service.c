/* SPDX-FileCopyrightText: 2026 ShinkoNet */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/app_blob.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pbl/services/filesystem/pfs.h"
#include "pbl/util/crc32.h"

#define APP_BLOB_FILE_NAME_MAX_LENGTH sizeof("ab000102030405060708090a0b0c0d0e0f")
#define APP_BLOB_MAGIC 0x4d4c4241u /* "ABLM" in little endian. */
#define APP_BLOB_VERSION 1u

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t version;
  uint32_t size;
  uint32_t size_inverse;
  uint32_t crc32;
  uint32_t crc32_inverse;
} AppBlobHeader;

static status_t prv_get_file_name(char *name, size_t name_size, const Uuid *uuid) {
  if (!name || !uuid) {
    return E_INVALID_ARGUMENT;
  }

  const uint8_t *b = (const uint8_t *)uuid;
  int length = snprintf(name, name_size,
                        "ab%02x%02x%02x%02x%02x%02x%02x%02x"
                        "%02x%02x%02x%02x%02x%02x%02x%02x",
                        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return (length > 0 && (size_t)length < name_size) ? S_SUCCESS : E_RANGE;
}

static bool prv_header_valid(const AppBlobHeader *header, size_t file_size) {
  return header->magic == APP_BLOB_MAGIC &&
         header->version == APP_BLOB_VERSION &&
         header->size >= 0x150u && header->size <= APP_BLOB_MAX_SIZE &&
         header->size_inverse == ~header->size &&
         header->crc32_inverse == ~header->crc32 &&
         file_size == sizeof(*header) + header->size;
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
  if (pfs_seek(fd, 0, FSeekSet) < 0 ||
      pfs_read(fd, header_out, sizeof(*header_out)) != (int)sizeof(*header_out)) {
    return E_ERROR;
  }
  return prv_header_valid(header_out, pfs_get_file_size(fd)) ? S_SUCCESS : E_DOES_NOT_EXIST;
}

status_t app_blob_service_get_info(const Uuid *uuid, AppBlobInfo *info_out) {
  if (!uuid || !info_out) {
    return E_INVALID_ARGUMENT;
  }

  int fd;
  status_t status = prv_open(uuid, OP_FLAG_READ, &fd);
  if (FAILED(status)) {
    return status;
  }

  AppBlobHeader header;
  status = prv_read_header(fd, &header);
  pfs_close(fd);
  if (FAILED(status)) {
    return status;
  }

  *info_out = (AppBlobInfo) {
    .size = header.size,
    .crc32 = header.crc32,
  };
  return S_SUCCESS;
}

size_t app_blob_service_get_free_size(void) {
  return get_available_pfs_space();
}

status_t app_blob_service_begin(const Uuid *uuid, uint32_t size) {
  if (!uuid || size < 0x150u || size > APP_BLOB_MAX_SIZE) {
    return E_INVALID_ARGUMENT;
  }

  char name[APP_BLOB_FILE_NAME_MAX_LENGTH];
  status_t status = prv_get_file_name(name, sizeof(name), uuid);
  if (FAILED(status)) {
    return status;
  }

  size_t old_size = 0;
  int old_fd = pfs_open(name, OP_FLAG_READ, FILE_TYPE_STATIC, 0);
  if (old_fd >= 0) {
    old_size = pfs_get_file_size(old_fd);
    pfs_close(old_fd);
  }

  const size_t required_size = sizeof(AppBlobHeader) + size;
  if (get_available_pfs_space() + old_size < required_size) {
    return E_OUT_OF_STORAGE;
  }

  if (old_fd >= 0) {
    status = pfs_remove(name);
    if (FAILED(status)) {
      return status;
    }
  }

  int fd = pfs_open(name, OP_FLAG_READ | OP_FLAG_WRITE, FILE_TYPE_STATIC, required_size);
  if (fd < 0) {
    return fd;
  }
  return pfs_close(fd);
}

int app_blob_service_write(const Uuid *uuid, uint32_t offset, const void *data, size_t size) {
  if (!uuid || (!data && size != 0) || size > 1024u) {
    return E_INVALID_ARGUMENT;
  }

  int fd;
  status_t status = prv_open(uuid, OP_FLAG_READ | OP_FLAG_WRITE, &fd);
  if (FAILED(status)) {
    return status;
  }

  AppBlobHeader header;
  status = prv_read_header(fd, &header);
  if (PASSED(status)) {
    pfs_close(fd);
    return E_INVALID_OPERATION;
  }
  if (status != E_DOES_NOT_EXIST) {
    pfs_close(fd);
    return status;
  }

  size_t file_size = pfs_get_file_size(fd);
  if (file_size < sizeof(AppBlobHeader) || offset > file_size - sizeof(AppBlobHeader) ||
      size > file_size - sizeof(AppBlobHeader) - offset) {
    pfs_close(fd);
    return E_RANGE;
  }

  if (pfs_seek(fd, (int)(sizeof(AppBlobHeader) + offset), FSeekSet) < 0) {
    pfs_close(fd);
    return E_ERROR;
  }
  int written = pfs_write(fd, data, size);
  status_t close_status = pfs_close(fd);
  return (written == (int)size && PASSED(close_status)) ? written : E_ERROR;
}

status_t app_blob_service_commit(const Uuid *uuid, uint32_t expected_crc32) {
  if (!uuid) {
    return E_INVALID_ARGUMENT;
  }

  int fd;
  status_t status = prv_open(uuid, OP_FLAG_READ | OP_FLAG_WRITE, &fd);
  if (FAILED(status)) {
    return status;
  }

  size_t file_size = pfs_get_file_size(fd);
  if (file_size < sizeof(AppBlobHeader) + 0x150u ||
      file_size > sizeof(AppBlobHeader) + APP_BLOB_MAX_SIZE ||
      pfs_seek(fd, sizeof(AppBlobHeader), FSeekSet) < 0) {
    pfs_close(fd);
    return E_RANGE;
  }

  uint32_t crc = CRC32_INIT;
  uint8_t buffer[256];
  size_t remaining = file_size - sizeof(AppBlobHeader);
  while (remaining) {
    size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
    if (pfs_read(fd, buffer, chunk) != (int)chunk) {
      pfs_close(fd);
      return E_ERROR;
    }
    crc = crc32(crc, buffer, chunk);
    remaining -= chunk;
  }

  if (crc != expected_crc32) {
    pfs_close(fd);
    return E_ERROR;
  }

  uint32_t payload_size = (uint32_t)(file_size - sizeof(AppBlobHeader));
  const AppBlobHeader header = {
    .magic = APP_BLOB_MAGIC,
    .version = APP_BLOB_VERSION,
    .size = payload_size,
    .size_inverse = ~payload_size,
    .crc32 = crc,
    .crc32_inverse = ~crc,
  };
  if (pfs_seek(fd, 0, FSeekSet) < 0 ||
      pfs_write(fd, &header, sizeof(header)) != (int)sizeof(header)) {
    pfs_close(fd);
    return E_ERROR;
  }
  return pfs_close(fd);
}

int app_blob_service_read(const Uuid *uuid, uint32_t offset, void *data, size_t size) {
  if (!uuid || (!data && size != 0)) {
    return E_INVALID_ARGUMENT;
  }

  int fd;
  status_t status = prv_open(uuid, OP_FLAG_READ | OP_FLAG_USE_PAGE_CACHE, &fd);
  if (FAILED(status)) {
    return status;
  }

  AppBlobHeader header;
  status = prv_read_header(fd, &header);
  if (FAILED(status)) {
    pfs_close(fd);
    return status;
  }
  if (offset > header.size || size > header.size - offset) {
    pfs_close(fd);
    return E_RANGE;
  }

  if (pfs_seek(fd, (int)(sizeof(AppBlobHeader) + offset), FSeekSet) < 0) {
    pfs_close(fd);
    return E_ERROR;
  }
  int read = pfs_read(fd, data, size);
  pfs_close(fd);
  return read;
}

status_t app_blob_service_delete(const Uuid *uuid) {
  char name[APP_BLOB_FILE_NAME_MAX_LENGTH];
  status_t status = prv_get_file_name(name, sizeof(name), uuid);
  return FAILED(status) ? status : pfs_remove(name);
}
