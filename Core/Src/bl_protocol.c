/**
  * @file    bl_protocol.c
  * @brief   UART Firmware Update Protocol v1.0 - frame parser and command handling.
  *
  * Frame: SOF(0x5A) CMD SEQ LEN(2) PAYLOAD(LEN) CRC16(2), little-endian.
  * CRC-16/CCITT-FALSE covers CMD..PAYLOAD. Responses use CMD | 0x80 and
  * start with a status byte.
  *
  * Deviations from the v1.0 draft (agreed for this project):
  *  - UPDATE_COMMIT and UPDATE_ABORT return to CONNECTED (not IDLE), so the
  *    host can send BOOT_APP / RESET / a new UPDATE_BEGIN straight away.
  *  - A failed update (flash erase/write error, verify mismatch, record write
  *    error) is aborted on the device and returns to CONNECTED, waiting for
  *    commands. The boot record stays "in progress", so after any reset the
  *    device stays in the bootloader until a complete image is committed.
  *  - Host silent for BL_UPDATE_TIMEOUT_MS during an update: abort, back to IDLE.
  *  - UPDATE_DATA: next_offset advances by the data length; a block shorter
  *    than 256 bytes is only accepted as the last block of the image.
  */
#include <stdbool.h>
#include <string.h>
#include "bl_protocol.h"
#include "bl_config.h"
#include "bl_boot.h"
#include "bl_crc.h"
#include "bl_debug.h"
#include "bl_flash.h"
#include "bl_record.h"
#include "bl_uart.h"
#include "stm32l1xx_hal.h"

#define BL_SOF              0x5AU
#define BL_HDR_SIZE         5U    /* SOF CMD SEQ LEN(2) */
#define BL_FRAME_OVERHEAD   7U    /* header + CRC16 */
#define BL_MAX_FRAME        (BL_MAX_PAYLOAD + BL_FRAME_OVERHEAD)
#define BL_RESP_FLAG        0x80U
#define BL_PROGRESS_STEP    4096U /* log update progress every 4 KB */

/* Command codes (section 5.1) */
enum {
    CMD_CONNECT         = 0x01,
    CMD_GET_DEVICE_INFO = 0x02,
    CMD_GET_APP_INFO    = 0x03,
    CMD_UPDATE_BEGIN    = 0x10,
    CMD_UPDATE_DATA     = 0x11,
    CMD_UPDATE_VERIFY   = 0x12,
    CMD_UPDATE_COMMIT   = 0x13,
    CMD_UPDATE_ABORT    = 0x14,
    CMD_READ_MEMORY     = 0x20,
    CMD_BOOT_APP        = 0x30,
    CMD_RESET           = 0x31,
    CMD_NACK            = 0xFF,
};

/* Status codes (section 6) */
enum {
    ST_OK               = 0x00,
    ERR_FRAME_CRC       = 0x01,
    ERR_UNKNOWN_CMD     = 0x02,
    ERR_BAD_LENGTH      = 0x03,
    ERR_BAD_PARAM       = 0x04,
    ERR_BAD_STATE       = 0x05,
    ERR_BAD_ADDRESS     = 0x06,
    ERR_SEQUENCE        = 0x07,
    ERR_FLASH_ERASE     = 0x08,
    ERR_FLASH_WRITE     = 0x09,
    ERR_VERIFY          = 0x0A,
    ERR_VERSION         = 0x0B,
    ERR_NOT_PERMITTED   = 0x0C,
    ERR_HW_MISMATCH     = 0x0D,
    ERR_NO_APP          = 0x0E,
    ERR_INTERNAL        = 0x7F,
};

/* Session states (section 5.2), one bit each so the command table can hold a mask */
enum {
    STATE_IDLE      = 1U << 0,
    STATE_CONNECTED = 1U << 1,
    STATE_UPDATING  = 1U << 2,
    STATE_VERIFIED  = 1U << 3,
};

/* What to do after the response has been sent */
typedef enum {
    ACT_NONE,
    ACT_BOOT_APP,
    ACT_RESET,
} post_action_t;

/* Handler: req points to the request payload, resp to the response payload.
   Returns the response payload length (status byte included). */
typedef uint16_t (*cmd_handler_t)(const uint8_t *req, uint16_t len, uint8_t *resp);

typedef struct {
    uint8_t       cmd;
    uint8_t       allowed_states;
    cmd_handler_t handler;
} cmd_entry_t;

static uint8_t       state;
static post_action_t post_action;
static uint32_t      last_frame_tick;   /* last valid frame, for the update timeout */

/* Receiver */
static uint8_t  rx_frame[BL_MAX_FRAME];
static uint16_t rx_len;
static uint16_t rx_payload_len;
static uint32_t last_rx_tick;

/* Last response, kept for duplicate detection (section 4.4) */
static uint8_t  tx_frame[BL_MAX_FRAME];
static uint16_t tx_frame_len;
static bool     last_valid;
static uint8_t  last_cmd;
static uint8_t  last_seq;

/* Update in progress */
static bl_manifest_t upd_manifest;
static uint32_t      next_offset;
static uint32_t      prev_offset;
static bool          have_prev;
static uint32_t      page_buf[BL_BLOCK_SIZE / 4U];   /* SRAM source for half-page programming */

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t status_only(uint8_t *resp, uint8_t status)
{
    resp[0] = status;
    return 1;
}

/* Leave the update. The boot record is not touched: it still says
   "in progress", so the device keeps booting into the bootloader. */
static void update_abort(uint8_t new_state, const char *reason)
{
    bl_log("Update aborted: %s", reason);
    state = new_state;
    have_prev = false;
}

/* ---------------------------------------------------------------------------
 * Command handlers
 * ------------------------------------------------------------------------- */
static uint16_t cmd_connect(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    if (len != 2)
        return status_only(resp, ERR_BAD_LENGTH);
    if (req[0] != BL_PROTO_MAJOR)
        return status_only(resp, ERR_VERSION);   // minor versions are backwards compatible

    resp[0] = ST_OK;
    resp[1] = BL_PROTO_MAJOR;
    resp[2] = BL_PROTO_MINOR;
    put_u16(&resp[3], BL_MAX_PAYLOAD);
    resp[5] = 0;                     // security level 0: simple (CRC only)
    memset(&resp[6], 0, 16);         // nonce unused in the simple bootloader

    state = STATE_CONNECTED;
    return 22;
}

static uint16_t cmd_get_device_info(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    (void)req;
    if (len != 0)
        return status_only(resp, ERR_BAD_LENGTH);

    resp[0] = ST_OK;
    put_u32(&resp[1], (BL_VERSION_MAJOR << 24) | (BL_VERSION_MINOR << 16) | BL_VERSION_PATCH);
    put_u16(&resp[5], (uint16_t)HAL_GetDEVID());
    put_u16(&resp[7], (uint16_t)HAL_GetREVID());
    put_u32(&resp[9],  HAL_GetUIDw0());      // the L1 UID words are not contiguous,
    put_u32(&resp[13], HAL_GetUIDw1());      // the HAL getters know the offsets
    put_u32(&resp[17], HAL_GetUIDw2());
    put_u16(&resp[21], *(volatile uint16_t *)FLASHSIZE_BASE);
    put_u16(&resp[23], BL_HW_ID);
    put_u32(&resp[25], BL_APP_START);
    put_u32(&resp[29], BL_APP_MAX_SIZE);
    put_u16(&resp[33], BL_BLOCK_SIZE);
    return 35;
}

static uint16_t cmd_get_app_info(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    bl_boot_record_t rec;
    bl_app_state_t app;

    (void)req;
    if (len != 0)
        return status_only(resp, ERR_BAD_LENGTH);

    app = bl_app_check(&rec);
    resp[0] = ST_OK;
    resp[1] = (uint8_t)app;
    if (app == APP_NONE)
        return 2;
    memcpy(&resp[2], &rec.manifest, sizeof(rec.manifest));
    return 2 + sizeof(rec.manifest);
}

static uint16_t cmd_update_begin(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    bl_manifest_t m;

    if (len != sizeof(bl_manifest_t))
        return status_only(resp, ERR_BAD_LENGTH);
    memcpy(&m, req, sizeof(m));

    // All checks happen before anything is invalidated
    if (m.magic != BL_MANIFEST_MAGIC || m.manifest_version != BL_MANIFEST_VERSION) {
        bl_log("UPDATE_BEGIN: bad magic/version");
        return status_only(resp, ERR_BAD_PARAM);
    }
    if (m.flags & BL_FLAG_ENCRYPTED) {
        bl_log("UPDATE_BEGIN: encrypted images not supported");
        return status_only(resp, ERR_BAD_PARAM);
    }
    if (m.hw_id != BL_HW_ID) {
        bl_log("UPDATE_BEGIN: hw_id 0x%04X, expected 0x%04X", m.hw_id, BL_HW_ID);
        return status_only(resp, ERR_HW_MISMATCH);
    }
    if (m.load_address != BL_APP_START) {
        bl_log("UPDATE_BEGIN: load address 0x%08lX", m.load_address);
        return status_only(resp, ERR_BAD_ADDRESS);
    }
    if (m.image_size == 0U || m.image_size > BL_APP_MAX_SIZE) {
        bl_log("UPDATE_BEGIN: image size %lu", m.image_size);
        return status_only(resp, ERR_BAD_LENGTH);
    }

    // From here on the old application will not be started again
    if (!bl_record_write(&m, REC_IN_PROGRESS))
        return status_only(resp, ERR_FLASH_WRITE);

    upd_manifest = m;
    next_offset = 0;
    have_prev = false;
    state = STATE_UPDATING;
    bl_log("Update started: %lu bytes, version 0x%08lX, crc 0x%08lX",
           m.image_size, m.image_version, m.image_crc32);

    resp[0] = ST_OK;
    put_u16(&resp[1], BL_BLOCK_SIZE);
    return 3;
}

static uint16_t cmd_update_data(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    uint32_t offset, n;
    bl_flash_result_t res;

    if (len < 5U || len > 4U + BL_BLOCK_SIZE)
        return status_only(resp, ERR_BAD_LENGTH);
    offset = get_u32(req);
    n = len - 4U;

    if (have_prev && offset == prev_offset) {
        // Retransmission of a block that is already written
        resp[0] = ST_OK;
        put_u32(&resp[1], next_offset);
        return 5;
    }
    if (offset != next_offset) {
        bl_log("UPDATE_DATA: offset %lu, expected %lu", offset, next_offset);
        resp[0] = ERR_SEQUENCE;
        put_u32(&resp[1], next_offset);
        return 5;
    }
    if (offset + n > upd_manifest.image_size || (n < BL_BLOCK_SIZE && offset + n != upd_manifest.image_size)) {
        bl_log("UPDATE_DATA: bad length %lu at offset %lu", n, offset);
        return status_only(resp, ERR_BAD_LENGTH);
    }

    // Pad the last block with 0x00 (the erased value on STM32L1)
    memset(page_buf, 0, sizeof(page_buf));
    memcpy(page_buf, &req[4], n);

    res = bl_flash_write_page(BL_APP_START + offset, page_buf);
    if (res != BL_FLASH_OK) {
        bl_log("UPDATE_DATA: flash error %d at 0x%08lX", (int)res, BL_APP_START + offset);
        update_abort(STATE_CONNECTED, "flash error");
        switch (res) {
        case BL_FLASH_ERASE_FAILED: return status_only(resp, ERR_FLASH_ERASE);
        case BL_FLASH_BAD_ADDRESS:  return status_only(resp, ERR_BAD_ADDRESS);
        default:                    return status_only(resp, ERR_FLASH_WRITE);
        }
    }

    prev_offset = offset;
    have_prev = true;
    next_offset = offset + n;

    if ((next_offset % BL_PROGRESS_STEP) == 0U || next_offset == upd_manifest.image_size)
        bl_log("Written %lu / %lu bytes", next_offset, upd_manifest.image_size);

    resp[0] = ST_OK;
    put_u32(&resp[1], next_offset);
    return 5;
}

static uint16_t cmd_update_verify(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    uint32_t crc;

    (void)req;
    if (len != 0)
        return status_only(resp, ERR_BAD_LENGTH);
    if (next_offset < upd_manifest.image_size) {
        bl_log("UPDATE_VERIFY: image incomplete (%lu / %lu)", next_offset, upd_manifest.image_size);
        return status_only(resp, ERR_BAD_STATE);
    }

    crc = crc32_iso_hdlc((const uint8_t *)BL_APP_START, upd_manifest.image_size);
    if (crc != upd_manifest.image_crc32) {
        bl_log("UPDATE_VERIFY: CRC 0x%08lX, manifest 0x%08lX", crc, upd_manifest.image_crc32);
        update_abort(STATE_CONNECTED, "verify failed");
        return status_only(resp, ERR_VERIFY);
    }
    if (!bl_vectors_valid(BL_APP_START)) {
        update_abort(STATE_CONNECTED, "image has no valid vector table");
        return status_only(resp, ERR_VERIFY);
    }

    bl_log("Image verified, CRC 0x%08lX", crc);
    state = STATE_VERIFIED;
    resp[0] = ST_OK;
    put_u32(&resp[1], crc);
    return 5;
}

static uint16_t cmd_update_commit(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    (void)req;
    if (len != 0)
        return status_only(resp, ERR_BAD_LENGTH);

    if (!bl_record_write(&upd_manifest, REC_VALID)) {
        update_abort(STATE_CONNECTED, "boot record write failed");
        return status_only(resp, ERR_FLASH_WRITE);
    }

    bl_log("Update committed");
    state = STATE_CONNECTED;
    have_prev = false;
    return status_only(resp, ST_OK);
}

static uint16_t cmd_update_abort(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    (void)req;
    if (len != 0)
        return status_only(resp, ERR_BAD_LENGTH);

    update_abort(STATE_CONNECTED, "requested by host");
    return status_only(resp, ST_OK);
}

static uint16_t cmd_read_memory(const uint8_t *req, uint16_t len, uint8_t *resp)
{
#if BL_ENABLE_READ_MEMORY
    uint32_t addr, n;
    bool in_app, in_eeprom;

    if (len != 6)
        return status_only(resp, ERR_BAD_LENGTH);
    addr = get_u32(req);
    n = get_u16(&req[4]);
    if (n == 0U || n > BL_BLOCK_SIZE)
        return status_only(resp, ERR_BAD_LENGTH);

    // Written so that addr + n cannot overflow
    in_app    = addr >= BL_APP_START    && addr <= BL_APP_END    && n <= BL_APP_END - addr;
    in_eeprom = addr >= BL_EEPROM_START && addr <= BL_EEPROM_END && n <= BL_EEPROM_END - addr;
    if (!in_app && !in_eeprom)
        return status_only(resp, ERR_BAD_ADDRESS);

    resp[0] = ST_OK;
    memcpy(&resp[1], (const void *)addr, n);
    return (uint16_t)(1U + n);
#else
    (void)req;
    (void)len;
    return status_only(resp, ERR_NOT_PERMITTED);
#endif
}

static uint16_t cmd_boot_app(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    (void)req;
    if (len != 0)
        return status_only(resp, ERR_BAD_LENGTH);
    if (!bl_app_is_valid())
        return status_only(resp, ERR_NO_APP);

    post_action = ACT_BOOT_APP;
    return status_only(resp, ST_OK);
}

static uint16_t cmd_reset(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    (void)req;
    if (len != 0)
        return status_only(resp, ERR_BAD_LENGTH);

    post_action = ACT_RESET;
    return status_only(resp, ST_OK);
}

static const cmd_entry_t cmd_table[] = {
    { CMD_CONNECT,         STATE_IDLE | STATE_CONNECTED,                      cmd_connect },
    { CMD_GET_DEVICE_INFO, STATE_CONNECTED | STATE_UPDATING,                  cmd_get_device_info },
    { CMD_GET_APP_INFO,    STATE_CONNECTED,                                   cmd_get_app_info },
    { CMD_UPDATE_BEGIN,    STATE_CONNECTED,                                   cmd_update_begin },
    { CMD_UPDATE_DATA,     STATE_UPDATING,                                    cmd_update_data },
    { CMD_UPDATE_VERIFY,   STATE_UPDATING,                                    cmd_update_verify },
    { CMD_UPDATE_COMMIT,   STATE_VERIFIED,                                    cmd_update_commit },
    { CMD_UPDATE_ABORT,    STATE_UPDATING | STATE_VERIFIED,                   cmd_update_abort },
    { CMD_READ_MEMORY,     STATE_CONNECTED,                                   cmd_read_memory },
    { CMD_BOOT_APP,        STATE_IDLE | STATE_CONNECTED,                      cmd_boot_app },
    { CMD_RESET,           STATE_CONNECTED | STATE_UPDATING | STATE_VERIFIED, cmd_reset },
};

static uint16_t dispatch(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *resp)
{
    for (uint32_t i = 0; i < sizeof(cmd_table) / sizeof(cmd_table[0]); i++) {
        const cmd_entry_t *e = &cmd_table[i];

        if (e->cmd != cmd)
            continue;
        if ((e->allowed_states & state) == 0U)
            return status_only(resp, ERR_BAD_STATE);
        return e->handler(req, len, resp);
    }
    // Includes AUTH_* (0x40-0x4F): hosts treat this as "authentication not required"
    return status_only(resp, ERR_UNKNOWN_CMD);
}

/* ---------------------------------------------------------------------------
 * Framing
 * ------------------------------------------------------------------------- */

/* Fill in header and CRC around a payload already placed at f + BL_HDR_SIZE */
static uint16_t build_frame(uint8_t *f, uint8_t cmd, uint8_t seq, uint16_t payload_len)
{
    f[0] = BL_SOF;
    f[1] = cmd;
    f[2] = seq;
    put_u16(&f[3], payload_len);
    put_u16(&f[BL_HDR_SIZE + payload_len], crc16_ccitt(&f[1], 4U + payload_len, CRC16_INIT));
    return payload_len + BL_FRAME_OVERHEAD;
}

static void handle_frame(void)
{
    uint8_t  cmd = rx_frame[1];
    uint8_t  seq = rx_frame[2];
    uint16_t len = rx_payload_len;
    uint16_t rx_crc = get_u16(&rx_frame[BL_HDR_SIZE + len]);
    uint8_t  status;

    if (crc16_ccitt(&rx_frame[1], 4U + len, CRC16_INIT) != rx_crc) {
        // Built in its own buffer so the cached response survives
        uint8_t nack[BL_FRAME_OVERHEAD + 1];
        nack[BL_HDR_SIZE] = ERR_FRAME_CRC;
        bl_uart_send(nack, build_frame(nack, CMD_NACK, seq, 1));
        bl_log("Frame CRC error (cmd 0x%02X seq %u), NACK sent", cmd, seq);
        return;
    }

    last_frame_tick = HAL_GetTick();

    // Retransmission of the last executed request: resend, do not execute again.
    // CONNECT always executes so a new session never gets a stale answer.
    if (last_valid && cmd != CMD_CONNECT && cmd == last_cmd && seq == last_seq) {
        bl_uart_send(tx_frame, tx_frame_len);
        bl_log("Duplicate cmd 0x%02X seq %u, response repeated", cmd, seq);
        return;
    }

    post_action = ACT_NONE;
    uint16_t resp_len = dispatch(cmd, &rx_frame[BL_HDR_SIZE], len, &tx_frame[BL_HDR_SIZE]);
    tx_frame_len = build_frame(tx_frame, cmd | BL_RESP_FLAG, seq, resp_len);
    last_cmd = cmd;
    last_seq = seq;
    last_valid = true;

    // Respond first, log afterwards so the debug output never delays the host
    bl_uart_send(tx_frame, tx_frame_len);

    status = tx_frame[BL_HDR_SIZE];
    if (cmd != CMD_UPDATE_DATA || status != ST_OK)
        bl_log("cmd 0x%02X seq %u len %u -> status 0x%02X", cmd, seq, len, status);

    switch (post_action) {
    case ACT_BOOT_APP:
        bl_jump_to_app();
        break;
    case ACT_RESET:
        bl_log("Resetting");
        NVIC_SystemReset();
        break;
    default:
        break;
    }
}

void bl_protocol_init(void)
{
    state = STATE_IDLE;
    post_action = ACT_NONE;
    rx_len = 0;
    last_valid = false;
    have_prev = false;
}

void bl_protocol_poll(void)
{
    uint8_t b;

    while (bl_uart_getc(&b)) {
        last_rx_tick = HAL_GetTick();

        if (rx_len == 0U && b != BL_SOF)
            continue;                       // hunt for SOF

        rx_frame[rx_len++] = b;

        if (rx_len == BL_HDR_SIZE) {
            rx_payload_len = get_u16(&rx_frame[3]);
            if (rx_payload_len > BL_MAX_PAYLOAD) {
                bl_log("Frame LEN %u too large, dropped", rx_payload_len);
                rx_len = 0;                 // reject, resume hunting
                continue;
            }
        }

        if (rx_len >= BL_HDR_SIZE && rx_len == rx_payload_len + BL_FRAME_OVERHEAD) {
            handle_frame();
            rx_len = 0;
        }
    }

    // Inter-byte timeout: drop a partial frame
    if (rx_len > 0U && (HAL_GetTick() - last_rx_tick) > BL_INTERBYTE_TIMEOUT_MS) {
        bl_log("Partial frame (%u bytes) timed out", rx_len);
        rx_len = 0;
    }

    // Host vanished in the middle of an update
    if ((state & (STATE_UPDATING | STATE_VERIFIED)) != 0U &&
        (HAL_GetTick() - last_frame_tick) > BL_UPDATE_TIMEOUT_MS)
        update_abort(STATE_IDLE, "host timeout");
}
