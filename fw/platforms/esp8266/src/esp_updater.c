/*
 * Copyright (c) 2016 Cesanta Software Limited
 * All rights reserved
 *
 * Implements mg_upd interface defined in mgos_updater_hal.h
 */

#include <inttypes.h>
#include <strings.h>
#include <user_interface.h>

#include "common/platforms/esp8266/esp_missing_includes.h"
#include "common/platforms/esp8266/rboot/rboot/appcode/rboot-api.h"
#include "common/queue.h"
#include "common/spiffs/spiffs.h"
#include "fw/src/mgos_console.h"
#include "fw/src/mgos_hal.h"
#include "fw/src/mgos_sys_config.h"
#include "fw/src/mgos_updater_rpc.h"
#include "fw/src/mgos_updater_hal.h"
#include "fw/src/mgos_updater_util.h"
#include "fw/platforms/esp8266/src/esp_fs.h"

#define SHA1SUM_LEN 40
#define FW_SLOT_SIZE 0x100000
#define UPDATER_MIN_BLOCK_SIZE 2048

struct file_info {
  char sha1_sum[40];
  char file_name[50];
  uint32_t size;
  spiffs_file file;
};

struct part_info {
  uint32_t addr;
  int size;
  int done;

  struct file_info fi;
};

struct mgos_upd_ctx {
  struct part_info fw_part;
  struct part_info fs_part;
  struct part_info fs_dir_part;

  int slot_to_write;
  struct part_info *current_part;
  uint32_t current_write_address;
  uint32_t erased_till;
  const char *status_msg;
};

rboot_config *get_rboot_config(void) {
  static rboot_config *cfg = NULL;
  if (cfg == NULL) {
    cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
      LOG(LL_DEBUG, ("Out of memory"));
      return NULL;
    }
    *cfg = rboot_get_config();
  }

  return cfg;
}

uint32_t get_fs_size(uint8_t rom) {
  return get_rboot_config()->fs_sizes[rom];
}

struct mgos_upd_ctx *mgos_upd_ctx_create(void) {
  return calloc(1, sizeof(struct mgos_upd_ctx));
}

const char *mgos_upd_get_status_msg(struct mgos_upd_ctx *ctx) {
  return ctx->status_msg;
}

static int fill_file_part_info(struct mgos_upd_ctx *ctx, struct json_token *tok,
                               const char *part_name, struct part_info *pi) {
  uint32_t addr;
  struct json_token sha = JSON_INVALID_TOKEN;
  struct json_token src = JSON_INVALID_TOKEN;
  json_scanf(tok->ptr, tok->len, "{addr: %u, cs_sha1: %T, src: %T, size: %d}", &addr, &sha, &src, &pi->size);

  /*
   * manifest always contain relative addresses, we have to
   * convert them to absolute (+0x100000 for slot #1)
   */
  pi->addr = addr + ctx->slot_to_write * FW_SLOT_SIZE;
  LOG(LL_DEBUG, ("Writeing 0x%x -> 0x%x", addr, pi->addr));

  if (sha.len == 0) {
    CONSOLE_LOG(LL_ERROR, ("cs_sha1 token not found in manifest"));
    return -1;
  }
  memcpy(pi->fi.sha1_sum, sha.ptr, sizeof(pi->fi.sha1_sum));

  if (src.len <= 0 || src.len >= (int) sizeof(pi->fi.file_name)) {
    CONSOLE_LOG(LL_ERROR, ("src token not found in manifest"));
    return -1;
  }

  memcpy(pi->fi.file_name, src.ptr, src.len);

  LOG(LL_DEBUG,
      ("Part %s : addr: %X sha1: %.*s src: %s", part_name, (int) pi->addr,
       sizeof(pi->fi.sha1_sum), pi->fi.sha1_sum, pi->fi.file_name));

  return 1;
}

void bin2hex(const uint8_t *src, int src_len, char *dst);

int verify_checksum(uint32_t addr, size_t len, const char *provided_checksum);

int mgos_upd_begin(struct mgos_upd_ctx *ctx, struct json_token *parts) {
  const rboot_config *cfg = get_rboot_config();
  struct json_token fs = JSON_INVALID_TOKEN, fw = JSON_INVALID_TOKEN,
                    fs_dir = JSON_INVALID_TOKEN;
  if (cfg == NULL) {
    ctx->status_msg = "Failed to get rBoot config";
    return -1;
  }

  json_scanf(parts->ptr, parts->len, "{fw: %T, fs: %T, fs_dir: %T}", &fw, &fs,
             &fs_dir);

  ctx->slot_to_write = (cfg->current_rom == 0 ? 1 : 0);
  LOG(LL_DEBUG, ("Slot to write: %d", ctx->slot_to_write));

  int fw_part_present =
      (fill_file_part_info(ctx, &fw, "fw", &ctx->fw_part) >= 0);

  if (!fw_part_present) {
    ctx->status_msg = "Firmware part is missing";
    return -1;
  }

  if (fill_file_part_info(ctx, &fs, "fs", &ctx->fs_part) < 0) {
    ctx->status_msg = "FS part is missing";
    return -1;
  }

  return 1;
}

int verify_checksum(uint32_t addr, size_t len, const char *provided_checksum) {
  uint8_t read_buf[4 * 100];
  char written_checksum[50];
  int to_read;

  cs_sha1_ctx ctx;
  cs_sha1_init(&ctx);

  size_t len_tmp = len;
  uint32 addr_tmp = addr;
  while (len != 0) {
    if (len > sizeof(read_buf)) {
      to_read = sizeof(read_buf);
    } else {
      to_read = len;
    }

    if (spi_flash_read(addr, (uint32_t *) read_buf, to_read) != 0) {
      CONSOLE_LOG(LL_ERROR, ("Failed to read %d bytes from %X", to_read, addr));
      return -1;
    }

    cs_sha1_update(&ctx, read_buf, to_read);
    addr += to_read;
    len -= to_read;

    mgos_wdt_feed();
  }

  cs_sha1_final(read_buf, &ctx);
  bin2hex(read_buf, 20, written_checksum);
  LOG(LL_DEBUG,
      ("SHA1 %u @ 0x%x = %.*s, want %.*s", len_tmp, addr_tmp, SHA1SUM_LEN,
       written_checksum, SHA1SUM_LEN, provided_checksum));

  if (strncasecmp(written_checksum, provided_checksum, SHA1SUM_LEN) != 0) {
    return -1;
  } else {
    return 1;
  }
}

static int prepare_to_write(struct mgos_upd_ctx *ctx,
                            const struct mgos_upd_file_info *fi,
                            struct part_info *part) {
  if (part->done != 0) {
    LOG(LL_DEBUG, ("Skipping %s", fi->name));
    return 0;
  }
  ctx->current_part = part;
  ctx->current_part->fi.size = fi->size;
  ctx->current_write_address = part->addr;
  ctx->erased_till = part->addr;
  /* See if current content is the same. */
  if (verify_checksum(part->addr, fi->size, part->fi.sha1_sum) == 1) {
    CONSOLE_LOG(LL_INFO,
                ("Digest matched, skipping %s %u @ 0x%x (%.*s)", fi->name,
                 fi->size, part->addr, SHA1SUM_LEN, part->fi.sha1_sum));
    part->done = 1;
    return 0;
  }
  CONSOLE_LOG(LL_INFO, ("Writing %s %u @ 0x%x (%.*s)", fi->name, fi->size,
                        part->addr, SHA1SUM_LEN, part->fi.sha1_sum));
  return 1;
}

enum mgos_upd_file_action mgos_upd_file_begin(
    struct mgos_upd_ctx *ctx, const struct mgos_upd_file_info *fi) {
  int ret;
  ctx->status_msg = "Failed to update file";
  LOG(LL_DEBUG, ("fi->name=%s", fi->name));
  if (strcmp(fi->name, ctx->fw_part.fi.file_name) == 0) {
    ret = prepare_to_write(ctx, fi, &ctx->fw_part);
  } else if (strcmp(fi->name, ctx->fs_part.fi.file_name) == 0) {
    ret = prepare_to_write(ctx, fi, &ctx->fs_part);
  } else {
    /* We need only fw & fs files, the rest just send to /dev/null */
    return MGOS_UPDATER_SKIP_FILE;
  }
  if (ret < 0) return MGOS_UPDATER_ABORT;
  return (ret == 0 ? MGOS_UPDATER_SKIP_FILE : MGOS_UPDATER_PROCESS_FILE);
}

static int prepare_flash(struct mgos_upd_ctx *ctx, uint32_t bytes_to_write) {
  while (ctx->current_write_address + bytes_to_write > ctx->erased_till) {
    uint32_t sec_no = ctx->erased_till / FLASH_SECTOR_SIZE;

    if ((ctx->erased_till % FLASH_ERASE_BLOCK_SIZE) == 0 &&
        ctx->current_part->addr + ctx->current_part->fi.size >=
            ctx->erased_till + FLASH_ERASE_BLOCK_SIZE) {
      LOG(LL_DEBUG, ("Erasing block @sector %X", sec_no));
      uint32_t block_no = ctx->erased_till / FLASH_ERASE_BLOCK_SIZE;
      if (SPIEraseBlock(block_no) != 0) {
        CONSOLE_LOG(LL_ERROR, ("Failed to erase flash block %X", block_no));
        return -1;
      }
      ctx->erased_till = (block_no + 1) * FLASH_ERASE_BLOCK_SIZE;
    } else {
      LOG(LL_DEBUG, ("Erasing sector %X", sec_no));
      if (spi_flash_erase_sector(sec_no) != 0) {
        CONSOLE_LOG(LL_ERROR, ("Failed to erase flash sector %X", sec_no));
        return -1;
      }
      ctx->erased_till = (sec_no + 1) * FLASH_SECTOR_SIZE;
    }
  }

  return 1;
}

int mgos_upd_file_data(struct mgos_upd_ctx *ctx,
                       const struct mgos_upd_file_info *fi,
                       struct mg_str data) {
  LOG(LL_DEBUG, ("File size: %u, received: %u to_write: %u", fi->size,
                 fi->processed, data.len));

  if (data.len < UPDATER_MIN_BLOCK_SIZE &&
      fi->size - fi->processed > UPDATER_MIN_BLOCK_SIZE) {
    return 0;
  }

  if (prepare_flash(ctx, data.len) < 0) {
    ctx->status_msg = "Failed to erase flash";
    return -1;
  }

  /* Write buffer size must be aligned to 4 */
  int bytes_processed = 0;
  uint32_t bytes_to_write_aligned = data.len & -4;
  if (bytes_to_write_aligned > 0) {
    LOG(LL_DEBUG, ("Writing %u bytes @%X", bytes_to_write_aligned,
                   ctx->current_write_address));

    if (spi_flash_write(ctx->current_write_address, (uint32_t *) data.p,
                        bytes_to_write_aligned) != 0) {
      ctx->status_msg = "Failed to write to flash";
      return -1;
    }
    data.p += bytes_to_write_aligned;
    data.len -= bytes_to_write_aligned;
    ctx->current_write_address += bytes_to_write_aligned;
    bytes_processed += bytes_to_write_aligned;
  }

  const uint32_t rest = fi->size - fi->processed - bytes_to_write_aligned;
  if (rest > 0 && rest < 4 && data.len >= 4) {
    /* File size is not aligned to 4, using align buf to write the tail */
    uint8_t align_buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(align_buf, data.p, rest);
    LOG(LL_DEBUG,
        ("Writing padded %u bytes @%X", rest, ctx->current_write_address));
    if (spi_flash_write(ctx->current_write_address, (uint32_t *) align_buf,
                        4) != 0) {
      ctx->status_msg = "Failed to write to flash";
      return -1;
    }
    bytes_processed += rest;
  }

  return bytes_processed;
}

int mgos_upd_file_end(struct mgos_upd_ctx *ctx,
                      const struct mgos_upd_file_info *fi, struct mg_str tail) {
  assert(tail.len == 0);
  if (verify_checksum(ctx->current_part->addr, fi->size,
                      ctx->current_part->fi.sha1_sum) < 0) {
    ctx->status_msg = "Invalid checksum";
    return -1;
  }
  ctx->current_part->done = 1;
  return tail.len;
}

int mgos_upd_finalize(struct mgos_upd_ctx *ctx) {
  if (!ctx->fw_part.done) {
    ctx->status_msg = "Missing fw part";
    return -1;
  }
  if (!ctx->fs_part.done && ctx->fs_dir_part.done == 0) {
    ctx->status_msg = "Missing fs part";
    return -2;
  }

  rboot_config *cfg = get_rboot_config();
  if (ctx->slot_to_write == cfg->current_rom) {
    LOG(LL_INFO, ("Using previous FW"));
    cfg->user_flags = 1;
    rboot_set_config(cfg);
    return 1;
  }

  cfg->previous_rom = cfg->current_rom;
  cfg->current_rom = ctx->slot_to_write;
  cfg->fs_addresses[cfg->current_rom] = ctx->fs_part.addr;
  cfg->fs_sizes[cfg->current_rom] = ctx->fs_part.fi.size;
  cfg->roms[cfg->current_rom] = ctx->fw_part.addr;
  cfg->roms_sizes[cfg->current_rom] = ctx->fw_part.fi.size;
  cfg->is_first_boot = 1;
  cfg->fw_updated = 1;
  cfg->user_flags = 1;
  cfg->boot_attempts = 0;
  rboot_set_config(cfg);

  LOG(LL_DEBUG,
      ("New rboot config: "
       "prev_rom: %d, current_rom: %d current_rom addr: %X, "
       "current_rom size: %d, current_fs addr: %X, current_fs size: %d",
       (int) cfg->previous_rom, (int) cfg->current_rom,
       cfg->roms[cfg->current_rom], cfg->roms_sizes[cfg->current_rom],
       cfg->fs_addresses[cfg->current_rom], cfg->fs_sizes[cfg->current_rom]));

  return 1;
}

void mgos_upd_ctx_free(struct mgos_upd_ctx *ctx) {
  free(ctx);
}

int mgos_upd_apply_update() {
  rboot_config *cfg = get_rboot_config();
  uint8_t spiffs_work_buf[LOG_PAGE_SIZE * 2];
  uint8_t spiffs_fds[32 * 2];
  spiffs old_fs;
  int ret = 0;
  uint32_t old_fs_size = cfg->fs_sizes[cfg->previous_rom];
  uint32_t old_fs_addr = cfg->fs_addresses[cfg->previous_rom];
  LOG(LL_INFO, ("Mounting old FS: %d @ 0x%x", old_fs_size, old_fs_addr));
  if (fs_mount(&old_fs, old_fs_addr, old_fs_size, spiffs_work_buf, spiffs_fds,
               sizeof(spiffs_fds))) {
    LOG(LL_ERROR, ("Update failed: cannot mount previous file system"));
    return -1;
  }

  ret = mgos_upd_merge_spiffs(&old_fs);

  SPIFFS_unmount(&old_fs);

  return ret;
}

void mgos_upd_boot_commit() {
  rboot_config *cfg = get_rboot_config();
  if (!cfg->fw_updated) return;
  LOG(LL_INFO, ("Committing ROM %d", cfg->current_rom));
  cfg->fw_updated = cfg->is_first_boot = 0;
  rboot_set_config(cfg);
}

void mgos_upd_boot_revert() {
  rboot_config *cfg = get_rboot_config();
  if (!cfg->fw_updated) return;
  LOG(LL_INFO, ("Update failed, reverting to ROM %d", cfg->previous_rom));
  cfg->current_rom = cfg->previous_rom;
  cfg->fw_updated = cfg->is_first_boot = 0;
  rboot_set_config(cfg);
}
