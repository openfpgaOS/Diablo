//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * phdp_proto.h — Pocket-Host Debug Protocol definitions
 *
 * Shared between phdpd (daemon) and phdp (CLI client).
 * See PHDP_SPEC.md for the full specification.
 */

#ifndef PHDP_PROTO_H
#define PHDP_PROTO_H

#include <stdint.h>

/* ── Wire protocol ──────────────────────────────────────────── */

#define PHDP_STX            0x02
#define PHDP_HEADER_SIZE    7       /* STX(1) + SEQ(1) + CMD(1) + LEN(4) */
#define PHDP_CRC_SIZE       2
#define PHDP_MAX_CHUNK      4096
#define PHDP_MAX_PACKET     (PHDP_HEADER_SIZE + PHDP_MAX_CHUNK + PHDP_CRC_SIZE)

/* Command / event IDs */
#define PHDP_EVT_BOOT_ALIVE     0x01    /* P -> H */
#define PHDP_CMD_CLIENT_READY   0x02    /* H -> P */
#define PHDP_REQ_OVERRIDE       0x10    /* P -> H */
#define PHDP_RES_STREAM         0x11    /* H -> P */
#define PHDP_RES_USE_SD         0x12    /* H -> P */
#define PHDP_DATA_CHUNK         0x20    /* H -> P */
#define PHDP_REPORT_PROGRESS    0x21    /* P -> H */
#define PHDP_CMD_NAK_RETRY      0x22    /* P -> H */
#define PHDP_MSG_CONSOLE_LOG    0x30    /* P -> H */
#define PHDP_EVT_EXEC_START     0x31    /* P -> H */
#define PHDP_CMD_SYS_RESET      0x40    /* H -> P */

/* ── Packet structure ───────────────────────────────────────── */

typedef struct {
    uint8_t  stx;
    uint8_t  seq;
    uint8_t  cmd;
    uint32_t len;           /* payload length, little-endian on wire */
} __attribute__((packed)) phdp_header_t;

/* ── CRC-16/CCITT ───────────────────────────────────────────── */

static inline uint16_t phdp_crc16(const uint8_t *data, uint32_t len) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

/* ── Packet build / parse helpers ───────────────────────────── */

/* Build a packet into buf. Returns total packet size. */
static inline int phdp_build_packet(uint8_t *buf, uint8_t seq, uint8_t cmd,
                                     const void *payload, uint32_t payload_len) {
    buf[0] = PHDP_STX;
    buf[1] = seq;
    buf[2] = cmd;
    buf[3] = (payload_len >>  0) & 0xFF;
    buf[4] = (payload_len >>  8) & 0xFF;
    buf[5] = (payload_len >> 16) & 0xFF;
    buf[6] = (payload_len >> 24) & 0xFF;
    if (payload_len > 0 && payload)
        __builtin_memcpy(buf + PHDP_HEADER_SIZE, payload, payload_len);
    uint32_t body_len = PHDP_HEADER_SIZE + payload_len;
    uint16_t crc = phdp_crc16(buf, body_len);
    buf[body_len + 0] = (crc >> 0) & 0xFF;
    buf[body_len + 1] = (crc >> 8) & 0xFF;
    return (int)(body_len + PHDP_CRC_SIZE);
}

/* Parse header from raw bytes. Returns 0 on success, -1 on bad STX. */
static inline int phdp_parse_header(const uint8_t *buf, phdp_header_t *hdr) {
    if (buf[0] != PHDP_STX) return -1;
    hdr->stx = buf[0];
    hdr->seq = buf[1];
    hdr->cmd = buf[2];
    hdr->len = (uint32_t)buf[3]
             | ((uint32_t)buf[4] << 8)
             | ((uint32_t)buf[5] << 16)
             | ((uint32_t)buf[6] << 24);
    return 0;
}

/* Validate CRC of a complete packet. Returns 0 if OK. */
static inline int phdp_validate_crc(const uint8_t *pkt, uint32_t total_len) {
    if (total_len < PHDP_HEADER_SIZE + PHDP_CRC_SIZE) return -1;
    uint32_t body_len = total_len - PHDP_CRC_SIZE;
    uint16_t expected = phdp_crc16(pkt, body_len);
    uint16_t got = (uint16_t)pkt[body_len] | ((uint16_t)pkt[body_len + 1] << 8);
    return (expected == got) ? 0 : -1;
}

/* ── IPC (daemon <-> CLI) ───────────────────────────────────── */

#define PHDP_SOCK_PATH      "/tmp/phdp.sock"
#define PHDP_LOG_RING_SIZE  (64 * 1024)     /* 64KB log ring buffer */

/* IPC command codes (CLI -> daemon) */
#define PHDP_IPC_STATUS     1
#define PHDP_IPC_PUSH       2
#define PHDP_IPC_CLEAR      3
#define PHDP_IPC_RESET      4
#define PHDP_IPC_WAIT       5
#define PHDP_IPC_LOGS       6

/* IPC request header */
typedef struct {
    uint8_t  cmd;
    uint8_t  slot;          /* for PUSH / CLEAR */
    uint16_t reserved;
    uint32_t arg;           /* for LOGS: line count (0 = stream) */
    char     path[256];     /* for PUSH: file path */
} __attribute__((packed)) phdp_ipc_req_t;

/* Daemon states */
#define PHDP_STATE_DISCONNECTED  0
#define PHDP_STATE_LISTENING     1
#define PHDP_STATE_CONNECTED     2
#define PHDP_STATE_READY         3
#define PHDP_STATE_STREAMING     4
#define PHDP_STATE_MONITORING    5

/* IPC status response */
typedef struct {
    uint8_t  state;
    uint8_t  num_queued;
    uint16_t reserved;
    uint32_t bytes_sent;
    uint32_t bytes_total;
} __attribute__((packed)) phdp_ipc_status_t;

/* Max data slots */
#define PHDP_MAX_SLOTS      16

/* Timeouts (ms) */
#define PHDP_DISCOVERY_TIMEOUT_MS   1000
#define PHDP_OVERRIDE_TIMEOUT_MS    200
#define PHDP_CHUNK_TIMEOUT_MS       1000
#define PHDP_MAX_RETRIES            5

/* Default baud rate */
#define PHDP_DEFAULT_BAUD   2000000

#endif /* PHDP_PROTO_H */
