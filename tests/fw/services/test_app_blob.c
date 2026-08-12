/* SPDX-FileCopyrightText: 2026 ShinkoNet */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>
#include <string.h>

#include "clar.h"

#include "pbl/services/filesystem/pfs.h"
#include "pbl/util/crc32.h"
#include "services/app_blob/service.h"

#include "fake_rtc.h"
#include "fake_spi_flash.h"
#include "stubs_analytics.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_print.h"
#include "stubs_prompt.h"
#include "stubs_serial.h"
#include "stubs_sleep.h"
#include "stubs_task_watchdog.h"

static const Uuid s_uuid = UuidMake(0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                                    0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f);

static uint32_t prv_crc(const void *data, size_t size) {
  return crc32(CRC32_INIT, data, size);
}

static void prv_store(const void *data, size_t size) {
  cl_assert_equal_i(app_blob_service_begin(&s_uuid, PebbleTask_App, size), S_SUCCESS);
  if (size) {
    cl_assert_equal_i(app_blob_service_write(&s_uuid, PebbleTask_App, 0, data, size), size);
  }
  cl_assert_equal_i(app_blob_service_commit(&s_uuid, PebbleTask_App, prv_crc(data, size)),
                    S_SUCCESS);
}

static void prv_assert_blob(const void *expected, size_t size) {
  AppBlobInfo info;
  cl_assert_equal_i(app_blob_service_get_info(&s_uuid, &info), S_SUCCESS);
  cl_assert_equal_i(info.size, size);
  cl_assert_equal_i(info.crc32, prv_crc(expected, size));

  if (size) {
    uint8_t actual[size];
    cl_assert_equal_i(app_blob_service_read(&s_uuid, 0, actual, size), size);
    cl_assert_equal_i(memcmp(actual, expected, size), 0);
  } else {
    cl_assert_equal_i(app_blob_service_read(&s_uuid, 0, NULL, 0), 0);
  }
}

void test_app_blob__initialize(void) {
  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);
  pfs_format(true);
  app_blob_service_init();
}

void test_app_blob__cleanup(void) {
  app_blob_service_process_cleanup(&s_uuid, PebbleTask_App);
  fake_spi_flash_cleanup();
}

void test_app_blob__supports_generic_small_and_empty_payloads(void) {
  const uint8_t small[] = {0xde, 0xad, 0xbe, 0xef};
  prv_store(small, sizeof(small));
  prv_assert_blob(small, sizeof(small));

  prv_store(NULL, 0);
  prv_assert_blob(NULL, 0);
}

void test_app_blob__reuses_reads_until_process_cleanup(void) {
  const uint8_t payload[] = "cached read session";
  uint8_t actual[sizeof(payload)] = {0};
  prv_store(payload, sizeof(payload));

  cl_assert_equal_i(app_blob_service_test_get_read_fd(), -1);
  cl_assert_equal_i(app_blob_service_read(&s_uuid, 0, actual, 6), 6);
  const int read_fd = app_blob_service_test_get_read_fd();
  cl_assert(read_fd >= 0);

  cl_assert_equal_i(app_blob_service_read(&s_uuid, 6, actual + 6,
                                          sizeof(payload) - 6),
                    sizeof(payload) - 6);
  cl_assert_equal_i(app_blob_service_test_get_read_fd(), read_fd);
  cl_assert_equal_i(memcmp(actual, payload, sizeof(payload)), 0);

  AppBlobInfo info;
  cl_assert_equal_i(app_blob_service_get_info(&s_uuid, &info), S_SUCCESS);
  cl_assert_equal_i(info.size, sizeof(payload));
  cl_assert_equal_i(app_blob_service_test_get_read_fd(), read_fd);

  app_blob_service_process_cleanup(&s_uuid, PebbleTask_App);
  cl_assert_equal_i(app_blob_service_test_get_read_fd(), -1);
  cl_assert_equal_i(app_blob_service_read(&s_uuid, 0, actual, sizeof(actual)),
                    sizeof(actual));
}

void test_app_blob__replacement_is_staged_until_valid_commit(void) {
  const uint8_t original[] = "original";
  const uint8_t replacement[] = "replacement";
  prv_store(original, sizeof(original));

  cl_assert_equal_i(app_blob_service_begin(&s_uuid, PebbleTask_App, sizeof(replacement)),
                    S_SUCCESS);
  cl_assert_equal_i(
      app_blob_service_write(&s_uuid, PebbleTask_App, 0, replacement, sizeof(replacement)),
      sizeof(replacement));

  prv_assert_blob(original, sizeof(original));
  cl_assert_equal_i(app_blob_service_commit(&s_uuid, PebbleTask_App, 0), E_ERROR);
  prv_assert_blob(original, sizeof(original));

  cl_assert_equal_i(
      app_blob_service_commit(&s_uuid, PebbleTask_App, prv_crc(replacement, sizeof(replacement))),
      S_SUCCESS);
  prv_assert_blob(replacement, sizeof(replacement));
}

void test_app_blob__transaction_is_owned_by_the_starting_task(void) {
  const uint8_t original[] = "original";
  const uint8_t replacement[] = "replacement";
  prv_store(original, sizeof(original));

  cl_assert_equal_i(app_blob_service_begin(&s_uuid, PebbleTask_App, sizeof(replacement)),
                    S_SUCCESS);
  cl_assert_equal_i(
      app_blob_service_write(&s_uuid, PebbleTask_Worker, 0, replacement, sizeof(replacement)),
      E_BUSY);
  cl_assert_equal_i(app_blob_service_commit(&s_uuid, PebbleTask_Worker,
                                            prv_crc(replacement, sizeof(replacement))),
                    E_BUSY);
  cl_assert_equal_i(app_blob_service_begin(&s_uuid, PebbleTask_Worker, sizeof(replacement)),
                    E_BUSY);
  cl_assert_equal_i(app_blob_service_delete_for_task(&s_uuid, PebbleTask_Worker), E_BUSY);

  app_blob_service_process_cleanup(&s_uuid, PebbleTask_Worker);
  cl_assert_equal_i(
      app_blob_service_write(&s_uuid, PebbleTask_App, 0, replacement, sizeof(replacement)),
      sizeof(replacement));

  app_blob_service_process_cleanup(&s_uuid, PebbleTask_App);
  prv_assert_blob(original, sizeof(original));
}

void test_app_blob__reboot_preserves_committed_blob_and_cleans_initial_staging(void) {
  const uint8_t original[] = "original";
  const uint8_t replacement[] = "replacement";
  prv_store(original, sizeof(original));

  cl_assert_equal_i(app_blob_service_begin(&s_uuid, PebbleTask_App, sizeof(replacement)),
                    S_SUCCESS);
  cl_assert_equal_i(
      app_blob_service_write(&s_uuid, PebbleTask_App, 0, replacement, sizeof(replacement)),
      sizeof(replacement));

  pfs_init(false);
  app_blob_service_init();
  prv_assert_blob(original, sizeof(original));

  cl_assert_equal_i(app_blob_service_delete(&s_uuid), S_SUCCESS);
  cl_assert_equal_i(app_blob_service_begin(&s_uuid, PebbleTask_App, sizeof(replacement)),
                    S_SUCCESS);
  cl_assert_equal_i(
      app_blob_service_write(&s_uuid, PebbleTask_App, 0, replacement, sizeof(replacement)),
      sizeof(replacement));

  pfs_init(false);
  app_blob_service_init();
  AppBlobInfo info;
  cl_assert_equal_i(app_blob_service_get_info(&s_uuid, &info), E_DOES_NOT_EXIST);

  cl_assert_equal_i(app_blob_service_begin(&s_uuid, PebbleTask_App, sizeof(replacement)),
                    S_SUCCESS);
  app_blob_service_process_cleanup(&s_uuid, PebbleTask_App);
}

void test_app_blob__reserves_filesystem_headroom(void) {
  int fd = pfs_open("space-pressure", OP_FLAG_WRITE, FILE_TYPE_STATIC, 8u * 1024u * 1024u);
  cl_assert(fd >= 0);
  cl_assert_equal_i(pfs_close(fd), S_SUCCESS);

  const size_t available = get_available_pfs_space();
  const size_t blob_free = app_blob_service_get_free_size();
  cl_assert(blob_free < APP_BLOB_MAX_SIZE);
  cl_assert(available > blob_free);
  cl_assert(available - blob_free >= 1024u * 1024u);
  cl_assert_equal_i(app_blob_service_begin(&s_uuid, PebbleTask_App, blob_free + 1),
                    E_OUT_OF_STORAGE);
}

void test_app_blob__allows_an_eight_megabyte_payload_when_space_is_available(void) {
  cl_assert_equal_i(app_blob_service_get_free_size(), APP_BLOB_MAX_SIZE);
  cl_assert_equal_i(app_blob_service_begin(&s_uuid, PebbleTask_App, APP_BLOB_MAX_SIZE), S_SUCCESS);
  app_blob_service_process_cleanup(&s_uuid, PebbleTask_App);
}
