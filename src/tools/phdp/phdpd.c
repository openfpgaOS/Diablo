//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * phdpd.c — Pocket-Host Debug Protocol daemon
 *
 * Single-threaded event loop owning the UART connection and all protocol
 * state.  Communicates with CLI clients via a Unix domain socket.
 *
 * Usage: phdpd [-d /dev/ttyUSB0] [-b 2000000] [-v]
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <termios.h>

#include "phdp_proto.h"

/* ── Compile-time limits ────────────────────────────────────── */

#define MAX_CLIENTS         8
#define PUSH_QUEUE_PATH     "/tmp/phdp_queue.dat"
#define UART_RX_BUF_SIZE    (PHDP_MAX_PACKET * 4)
#define LOG_LINE_MAX        512

/* ── Push queue entry ───────────────────────────────────────── */

typedef struct {
    uint8_t  slot_id;
    bool     active;
    char     path[256];
} push_entry_t;

/* ── Streaming state (active transfer) ──────────────────────── */

typedef struct {
    int      fd;                /* open file descriptor */
    uint8_t  slot_id;
    uint32_t file_size;
    uint32_t bytes_sent;        /* bytes sent to wire */
    uint32_t bytes_acked;       /* bytes confirmed by Pocket */
    uint16_t chunk_size;        /* negotiated chunk size */
    uint8_t  seq;               /* next sequence number */
    int      retries;           /* consecutive retry count */
    bool     waiting_ack;       /* waiting for REPORT_PROGRESS */
    int64_t  last_chunk_ms;     /* timestamp of last chunk sent */
} stream_state_t;

/* ── Log ring buffer ────────────────────────────────────────── */

typedef struct {
    char     buf[PHDP_LOG_RING_SIZE];
    uint32_t head;              /* write position */
    uint32_t len;               /* bytes used (up to PHDP_LOG_RING_SIZE) */
} log_ring_t;

/* ── IPC client tracking ────────────────────────────────────── */

typedef struct {
    int  fd;
    bool wait_exec;             /* PHDP_IPC_WAIT: waiting for EVT_EXEC_START */
    bool stream_logs;           /* PHDP_IPC_LOGS with arg=0: live tail */
} ipc_client_t;

/* ── Global daemon state ────────────────────────────────────── */

static struct {
    /* Configuration */
    const char *uart_path;
    int         baud;
    bool        verbose;        /* -v: state, packet headers, stream progress */
    bool        trace;          /* -t: -v + hex dumps + IPC events + timestamps */

    /* State machine */
    int         state;

    /* UART */
    int         uart_fd;
    uint8_t     uart_rx[UART_RX_BUF_SIZE];
    uint32_t    uart_rx_len;

    /* Pocket capabilities (from EVT_BOOT_ALIVE) */
    uint32_t    core_id;
    uint16_t    fw_version;
    uint16_t    max_chunk;

    /* Push queue */
    push_entry_t push_queue[PHDP_MAX_SLOTS];

    /* Active stream */
    stream_state_t stream;
    bool           streaming_active;
    int            monitoring_flush_pending;

    /* Log ring */
    log_ring_t  log_ring;

    /* IPC */
    int          listen_fd;
    ipc_client_t clients[MAX_CLIENTS];
    int          num_clients;

    /* Sequence counter for outgoing packets */
    uint8_t      tx_seq;
} G;

/* ── Helpers ────────────────────────────────────────────────── */

static volatile sig_atomic_t g_quit = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_quit = 1;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wall-clock relative timestamp prefix for trace output. The first
 * call latches a t0; everything after prints "[+12.345s] ". Keeping
 * the prefix relative makes log diffs across runs trivially comparable. */
static void ts_prefix(void) {
    static int64_t t0 = 0;
    if (!G.trace) return;
    int64_t now = now_ms();
    if (t0 == 0) t0 = now;
    double dt = (double)(now - t0) / 1000.0;
    fprintf(stderr, "[+%7.3fs] ", dt);
}

static void logv(const char *fmt, ...) {
    if (!G.verbose) return;
    ts_prefix();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* tracev() is the noisier sibling of logv(). Anything that would be
 * spam at -v level (per-byte UART traffic, hex dumps, IPC chatter)
 * goes through here so plain -v stays readable. */
static void tracev(const char *fmt, ...) {
    if (!G.trace) return;
    ts_prefix();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Hex dump for trace output. Prints up to `len` bytes as
 *   tag: 02 1F 20 04 00 00 00  ..len.
 * Truncates at TRACE_DUMP_MAX so a 4 KB stream chunk doesn't fill
 * the terminal — the header + first/last bytes are usually enough
 * to diagnose framing problems. */
#define TRACE_DUMP_MAX 32
static void hex_dump(const char *tag, const uint8_t *buf, uint32_t len) {
    if (!G.trace) return;
    ts_prefix();
    fprintf(stderr, "%s [%u]:", tag, len);
    uint32_t n = len < TRACE_DUMP_MAX ? len : TRACE_DUMP_MAX;
    for (uint32_t i = 0; i < n; i++)
        fprintf(stderr, " %02X", buf[i]);
    if (len > TRACE_DUMP_MAX) fprintf(stderr, " ...");
    fprintf(stderr, "\n");
}

static const char *state_name(int s) {
    switch (s) {
    case PHDP_STATE_DISCONNECTED: return "DISCONNECTED";
    case PHDP_STATE_LISTENING:    return "LISTENING";
    case PHDP_STATE_CONNECTED:    return "CONNECTED";
    case PHDP_STATE_READY:        return "READY";
    case PHDP_STATE_STREAMING:    return "STREAMING";
    case PHDP_STATE_MONITORING:   return "MONITORING";
    default:                      return "UNKNOWN";
    }
}

static void set_state(int new_state) {
    if (G.state != new_state) {
        logv("[state] %s -> %s\n", state_name(G.state), state_name(new_state));
        G.state = new_state;
    }
}

/* ── Push queue persistence ─────────────────────────────────── */

static void queue_save(void) {
    FILE *f = fopen(PUSH_QUEUE_PATH, "w");
    if (!f) return;
    for (int i = 0; i < PHDP_MAX_SLOTS; i++) {
        if (G.push_queue[i].active) {
            fprintf(f, "%u %s\n", G.push_queue[i].slot_id, G.push_queue[i].path);
        }
    }
    fclose(f);
}

static void queue_load(void) {
    FILE *f = fopen(PUSH_QUEUE_PATH, "r");
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        unsigned slot;
        char path[256];
        if (sscanf(line, "%u %255s", &slot, path) == 2 && slot < PHDP_MAX_SLOTS) {
            G.push_queue[slot].slot_id = (uint8_t)slot;
            G.push_queue[slot].active = true;
            snprintf(G.push_queue[slot].path, sizeof(G.push_queue[slot].path), "%s", path);
        }
    }
    fclose(f);
}

static int queue_count(void) {
    int n = 0;
    for (int i = 0; i < PHDP_MAX_SLOTS; i++)
        if (G.push_queue[i].active) n++;
    return n;
}

/* ── Log ring buffer ────────────────────────────────────────── */

static void log_ring_write(const char *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        uint32_t pos = G.log_ring.head;
        G.log_ring.buf[pos] = data[i];
        G.log_ring.head = (pos + 1) % PHDP_LOG_RING_SIZE;
        if (G.log_ring.len < PHDP_LOG_RING_SIZE)
            G.log_ring.len++;
    }
}

/*
 * Read up to max_lines from the tail of the ring buffer.
 * If max_lines == 0, return the entire ring contents.
 * Returns a malloc'd string the caller must free.
 */
/* Read from the log ring. Returns malloc'd buffer and sets *out_len.
 * If max_lines > 0, returns only the last N lines. */
static char *log_ring_read(uint32_t max_lines, uint32_t *out_len) {
    if (G.log_ring.len == 0) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    /* Determine start position of the ring data */
    uint32_t start;
    if (G.log_ring.len < PHDP_LOG_RING_SIZE)
        start = 0;
    else
        start = G.log_ring.head;  /* oldest byte is at head (wrapped) */

    /* Linearize the ring into a temporary buffer */
    char *linear = malloc(G.log_ring.len);
    for (uint32_t i = 0; i < G.log_ring.len; i++)
        linear[i] = G.log_ring.buf[(start + i) % PHDP_LOG_RING_SIZE];

    if (max_lines == 0) {
        if (out_len) *out_len = G.log_ring.len;
        return linear;
    }

    /* Find the last max_lines newlines */
    uint32_t count = 0;
    int pos = (int)G.log_ring.len - 1;
    while (pos >= 0 && count < max_lines) {
        if (linear[pos] == '\n') count++;
        pos--;
    }
    pos++;  /* points to start of the desired region */

    uint32_t result_len = G.log_ring.len - (uint32_t)pos;
    char *result = malloc(result_len);
    memcpy(result, linear + pos, result_len);
    free(linear);
    if (out_len) *out_len = result_len;
    return result;
}

/* Broadcast a log line to all streaming clients */
static void log_ring_broadcast(const char *line, uint32_t len) {
    for (int i = 0; i < G.num_clients; i++) {
        if (G.clients[i].stream_logs) {
            /* Best-effort write; don't block */
            (void)write(G.clients[i].fd, line, len);
        }
    }
}

/* ── UART ───────────────────────────────────────────────────── */

static speed_t baud_to_speed(int baud) {
    switch (baud) {
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 115200:  return B115200;
    case 230400:  return B230400;
    case 460800:  return B460800;
    case 500000:  return B500000;
    case 576000:  return B576000;
    case 921600:  return B921600;
    case 1000000: return B1000000;
    case 1152000: return B1152000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    case 2500000: return B2500000;
    case 3000000: return B3000000;
    case 3500000: return B3500000;
    case 4000000: return B4000000;
    default:      return B0;
    }
}

static int uart_open(void) {
    int fd = open(G.uart_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) { close(fd); return -1; }

    speed_t sp = baud_to_speed(G.baud);
    if (sp == B0) {
        fprintf(stderr, "unsupported baud rate %d\n", G.baud);
        close(fd);
        return -1;
    }

    cfsetispeed(&tty, sp);
    cfsetospeed(&tty, sp);

    /* Raw mode, 8N1 */
    cfmakeraw(&tty);
    tty.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
    tty.c_cflag |= CLOCAL | CREAD | CS8;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);

    return fd;
}

static void uart_close(void) {
    if (G.uart_fd >= 0) {
        close(G.uart_fd);
        G.uart_fd = -1;
    }
    G.uart_rx_len = 0;
}

static int uart_send(const uint8_t *data, int len) {
    hex_dump("[tx-bytes]", data, (uint32_t)len);
    int total = 0;
    while (total < len) {
        ssize_t n = write(G.uart_fd, data + total, (size_t)(len - total));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Brief spin — UART TX buffer should drain quickly at 2Mbaud */
                usleep(100);
                continue;
            }
            tracev("[tx-bytes] write error: %s\n", strerror(errno));
            return -1; /* EIO or device gone */
        }
        total += (int)n;
    }
    return 0;
}

static int uart_send_packet(uint8_t cmd, const void *payload, uint32_t payload_len) {
    uint8_t pkt[PHDP_MAX_PACKET];
    int pkt_len = phdp_build_packet(pkt, G.tx_seq++, cmd, payload, payload_len);
    tracev("[tx] cmd=0x%02X seq=%u len=%u\n", cmd, (unsigned)(G.tx_seq - 1), payload_len);
    return uart_send(pkt, pkt_len);
}

/* ── Streaming logic ────────────────────────────────────────── */

static void stream_abort(void) {
    if (G.streaming_active && G.stream.fd >= 0) {
        close(G.stream.fd);
        G.stream.fd = -1;
    }
    G.streaming_active = false;
}

static int stream_begin(uint8_t slot_id, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        close(fd);
        return -1;
    }

    uint32_t file_size = (uint32_t)st.st_size;
    /* Negotiate chunk size: use Pocket's max or PHDP_MAX_CHUNK, whichever smaller */
    uint16_t chunk = PHDP_MAX_CHUNK;
    if (G.max_chunk > 0 && G.max_chunk < chunk)
        chunk = G.max_chunk;

    memset(&G.stream, 0, sizeof(G.stream));
    G.stream.fd         = fd;
    G.stream.slot_id    = slot_id;
    G.stream.file_size  = file_size;
    G.stream.bytes_sent = 0;
    G.stream.bytes_acked = 0;
    G.stream.chunk_size = chunk;
    G.stream.seq        = G.tx_seq;
    G.stream.retries    = 0;
    G.stream.waiting_ack = false;
    G.stream.last_chunk_ms = 0;
    G.streaming_active  = true;

    /* Send RES_STREAM: total size (4B) + chunk size (2B) */
    uint8_t payload[6];
    payload[0] = (file_size >>  0) & 0xFF;
    payload[1] = (file_size >>  8) & 0xFF;
    payload[2] = (file_size >> 16) & 0xFF;
    payload[3] = (file_size >> 24) & 0xFF;
    payload[4] = (chunk >> 0) & 0xFF;
    payload[5] = (chunk >> 8) & 0xFF;

    logv("[stream] slot %u: %s (%u bytes, chunk %u)\n", slot_id, path, file_size, chunk);
    set_state(PHDP_STATE_STREAMING);

    if (uart_send_packet(PHDP_RES_STREAM, payload, 6) < 0) return -1;

    /* Give the Pocket time to transition from phdp_request_override()
     * to phdp_stream_slot() before we send the first DATA_CHUNK.
     * Without this delay, the first chunk bytes arrive before the
     * Pocket starts listening (no UART FIFO on the Pocket side). */
    usleep(100000);  /* 100ms */

    return 0;
}

static int stream_send_next_chunk(void) {
    if (!G.streaming_active) return 0;
    if (G.stream.waiting_ack) return 0;
    if (G.stream.bytes_sent >= G.stream.file_size) return 0;

    uint32_t remaining = G.stream.file_size - G.stream.bytes_sent;
    uint32_t to_send = remaining < G.stream.chunk_size ? remaining : G.stream.chunk_size;

    uint8_t pkt[PHDP_MAX_PACKET];
    uint8_t chunk_buf[PHDP_MAX_CHUNK];

    ssize_t n = read(G.stream.fd, chunk_buf, to_send);
    if (n <= 0) {
        fprintf(stderr, "read error during stream: %s\n", strerror(errno));
        stream_abort();
        set_state(PHDP_STATE_READY);
        return -1;
    }

    int pkt_len = phdp_build_packet(pkt, G.stream.seq++, PHDP_DATA_CHUNK,
                                    chunk_buf, (uint32_t)n);

    if (uart_send(pkt, pkt_len) < 0) return -1;

    G.stream.bytes_sent += (uint32_t)n;
    G.stream.waiting_ack = true;
    G.stream.last_chunk_ms = now_ms();
    G.stream.retries = 0;

    {
        unsigned pct = (unsigned)(G.stream.bytes_sent * 100 / G.stream.file_size);
        tracev("[stream] sent %u/%u (%u%%)\n", G.stream.bytes_sent, G.stream.file_size, pct);
    }

    return 0;
}

static int stream_retry_last_chunk(void) {
    if (!G.streaming_active) return 0;

    G.stream.retries++;
    if (G.stream.retries > PHDP_MAX_RETRIES) {
        fprintf(stderr, "max retries exceeded, aborting stream\n");
        stream_abort();
        set_state(PHDP_STATE_READY);
        return -1;
    }

    /* Seek back by the amount we last sent */
    /* bytes_sent already advanced; need to re-send the last chunk */
    uint32_t chunk_len = G.stream.chunk_size;
    if (G.stream.bytes_sent < chunk_len)
        chunk_len = G.stream.bytes_sent;

    /* Seek back */
    G.stream.bytes_sent -= chunk_len;
    if (lseek(G.stream.fd, G.stream.bytes_sent, SEEK_SET) < 0) {
        stream_abort();
        set_state(PHDP_STATE_READY);
        return -1;
    }

    /* Rewind sequence number too */
    G.stream.seq--;
    G.stream.waiting_ack = false;

    logv("[stream] retrying chunk (attempt %d/%d)\n", G.stream.retries, PHDP_MAX_RETRIES);
    return stream_send_next_chunk();
}

/* ── Incoming UART packet handling ──────────────────────────── */

static void handle_evt_boot_alive(const uint8_t *payload, uint32_t len) {
    if (len < 8) return;

    G.core_id    = (uint32_t)payload[0]
                 | ((uint32_t)payload[1] << 8)
                 | ((uint32_t)payload[2] << 16)
                 | ((uint32_t)payload[3] << 24);
    G.fw_version = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
    G.max_chunk  = (uint16_t)payload[6] | ((uint16_t)payload[7] << 8);

    logv("[pocket] BOOT_ALIVE core=0x%08X ver=%u.%u max_chunk=%u\n",
         G.core_id, G.fw_version >> 8, G.fw_version & 0xFF, G.max_chunk);

    /* If we were streaming, abort — Pocket rebooted */
    stream_abort();

    set_state(PHDP_STATE_CONNECTED);

    /* Respond with CMD_CLIENT_READY */
    if (uart_send_packet(PHDP_CMD_CLIENT_READY, NULL, 0) < 0) {
        uart_close();
        set_state(PHDP_STATE_DISCONNECTED);
        return;
    }

    set_state(PHDP_STATE_READY);
}

static void handle_req_override(const uint8_t *payload, uint32_t len) {
    if (len < 1) return;

    uint8_t slot_id = payload[0];
    logv("[pocket] REQ_OVERRIDE slot=%u\n", slot_id);

    /* Check push queue */
    if (slot_id < PHDP_MAX_SLOTS && G.push_queue[slot_id].active) {
        const char *path = G.push_queue[slot_id].path;
        if (stream_begin(slot_id, path) < 0) {
            /* Fallback to SD on error */
            uart_send_packet(PHDP_RES_USE_SD, NULL, 0);
        }
    } else {
        /* No override queued — use SD */
        uart_send_packet(PHDP_RES_USE_SD, NULL, 0);
    }
}

static void handle_report_progress(const uint8_t *payload, uint32_t len) {
    if (len < 4 || !G.streaming_active) return;

    uint32_t bytes_received = (uint32_t)payload[0]
                            | ((uint32_t)payload[1] << 8)
                            | ((uint32_t)payload[2] << 16)
                            | ((uint32_t)payload[3] << 24);

    G.stream.bytes_acked = bytes_received;
    G.stream.waiting_ack = false;
    G.stream.retries = 0;

    if (G.stream.bytes_sent >= G.stream.file_size) {
        /* Transfer complete — wait for EVT_EXEC_START */
        logv("[stream] transfer complete (%u bytes)\n", G.stream.file_size);
        close(G.stream.fd);
        G.stream.fd = -1;
        G.streaming_active = false;
        set_state(PHDP_STATE_READY);
    } else {
        /* Send next chunk */
        stream_send_next_chunk();
    }
}

static void handle_nak_retry(const uint8_t *payload, uint32_t len) {
    if (len < 1) return;
    uint8_t failed_seq = payload[0];
    logv("[pocket] NAK_RETRY seq=%u\n", failed_seq);
    stream_retry_last_chunk();
}

static void handle_console_log(const uint8_t *payload, uint32_t len) {
    if (len < 2) return;

    uint8_t level = payload[0];
    const char *msg = (const char *)payload + 1;
    uint32_t msg_len = len - 1;

    /* Format: "[L] message\n" */
    char line[LOG_LINE_MAX];
    int n = snprintf(line, sizeof(line), "[%u] %.*s\n", level, (int)msg_len, msg);
    if (n < 0) return;
    uint32_t line_len = (uint32_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);

    log_ring_write(line, line_len);
    log_ring_broadcast(line, line_len);

    /* Trace-only stderr mirror — the structured log already lands in
     * the client's `phdp logs` stream verbatim, so duplicating it on
     * phdpd's stderr at -v level just doubles every line on the
     * user's terminal. */
    tracev("[console-log] %.*s", (int)line_len, line);
}

static void handle_exec_start(const uint8_t *payload, uint32_t len) {
    uint32_t entry = 0;
    if (len >= 4) {
        entry = (uint32_t)payload[0]
              | ((uint32_t)payload[1] << 8)
              | ((uint32_t)payload[2] << 16)
              | ((uint32_t)payload[3] << 24);
    }

    logv("[pocket] EXEC_START entry=0x%08X\n", entry);
    stream_abort(); /* clean up if anything lingering */
    G.monitoring_flush_pending = 1;  /* flush RX buffer after uart_process_rx returns */

    set_state(PHDP_STATE_MONITORING);

    /* Notify any IPC clients waiting for exec.
     * 2026-04-08: success code is 0 — phdp.c cmd_wait treats
     * result==0 as EXIT_OK and anything else as "wait failed". Was
     * previously 1 which made every successful wait look like an
     * error to the client. */
    uint8_t ok = 0;
    for (int i = 0; i < G.num_clients; i++) {
        if (G.clients[i].wait_exec) {
            (void)write(G.clients[i].fd, &ok, 1);
            G.clients[i].wait_exec = false;
        }
    }
}

static void process_uart_packet(const uint8_t *pkt, uint32_t total_len) {
    phdp_header_t hdr;
    if (phdp_parse_header(pkt, &hdr) != 0) return;
    if (phdp_validate_crc(pkt, total_len) != 0) {
        logv("[uart] CRC error, dropping packet cmd=0x%02X\n", hdr.cmd);
        hex_dump("[crc-fail]", pkt, total_len);
        return;
    }

    tracev("[rx] cmd=0x%02X seq=%u len=%u\n", hdr.cmd, hdr.seq, hdr.len);
    hex_dump("[rx-bytes]", pkt, total_len);

    const uint8_t *payload = pkt + PHDP_HEADER_SIZE;

    switch (hdr.cmd) {
    case PHDP_EVT_BOOT_ALIVE:   handle_evt_boot_alive(payload, hdr.len);   break;
    case PHDP_REQ_OVERRIDE:     handle_req_override(payload, hdr.len);     break;
    case PHDP_REPORT_PROGRESS:  handle_report_progress(payload, hdr.len);  break;
    case PHDP_CMD_NAK_RETRY:    handle_nak_retry(payload, hdr.len);        break;
    case PHDP_MSG_CONSOLE_LOG:  handle_console_log(payload, hdr.len);      break;
    case PHDP_EVT_EXEC_START:   handle_exec_start(payload, hdr.len);       break;
    default:
        logv("[uart] unknown cmd 0x%02X, ignoring\n", hdr.cmd);
        break;
    }
}

/*
 * Try to extract complete packets from the UART receive buffer.
 * Scans for STX, validates header + CRC, dispatches.
 */
static void uart_process_rx(void) {
    /* First pass: drain any console bytes that have no PHDP STX in
     * sight. This is independent of the 9-byte PHDP header minimum
     * below -- a short trailing console line (e.g. the last status
     * print of a boot banner) must reach the user immediately, not
     * wait for the buffer to grow past PHDP_HEADER_SIZE+CRC. */
    {
        uint32_t scan = 0;
        while (scan < G.uart_rx_len && G.uart_rx[scan] != PHDP_STX)
            scan++;
        if (scan == G.uart_rx_len && scan > 0) {
            /* No STX anywhere -- entire buffer is console output. */
            tracev("[console-rx] %u bytes (no STX): %.*s\n", scan,
                   (int)(scan > 60 ? 60 : scan), (const char *)G.uart_rx);
            (void)!write(STDOUT_FILENO, G.uart_rx, scan);
            log_ring_write((const char *)G.uart_rx, scan);
            log_ring_broadcast((const char *)G.uart_rx, scan);
            G.uart_rx_len = 0;
            return;
        }
    }

    while (G.uart_rx_len >= PHDP_HEADER_SIZE + PHDP_CRC_SIZE) {
        /* Scan for STX */
        uint32_t offset = 0;
        while (offset < G.uart_rx_len && G.uart_rx[offset] != PHDP_STX)
            offset++;

        if (offset > 0) {
            /* Non-PHDP bytes before STX — treat as raw console output.
             * Forward bytes as-is to phdpd's stdout for the live screen
             * mirror, plus the log ring + broadcast for `phdp logs`.
             * The per-packet `[console-rx]` debug marker is trace-only. */
            tracev("[console-rx] %u bytes: %.*s\n", offset,
                   (int)(offset > 60 ? 60 : offset), (const char *)G.uart_rx);
            (void)!write(STDOUT_FILENO, G.uart_rx, offset);
            log_ring_write((const char *)G.uart_rx, offset);
            log_ring_broadcast((const char *)G.uart_rx, offset);
            G.uart_rx_len -= offset;
            memmove(G.uart_rx, G.uart_rx + offset, G.uart_rx_len);
        }

        if (G.uart_rx_len < PHDP_HEADER_SIZE + PHDP_CRC_SIZE)
            break;

        /* Parse header to get payload length */
        phdp_header_t hdr;
        if (phdp_parse_header(G.uart_rx, &hdr) != 0) {
            /* Bad STX after alignment — skip one byte */
            G.uart_rx_len--;
            memmove(G.uart_rx, G.uart_rx + 1, G.uart_rx_len);
            continue;
        }

        uint32_t total_len = PHDP_HEADER_SIZE + hdr.len + PHDP_CRC_SIZE;

        if (total_len > PHDP_MAX_PACKET) {
            /* Corrupt length — skip this STX */
            G.uart_rx_len--;
            memmove(G.uart_rx, G.uart_rx + 1, G.uart_rx_len);
            continue;
        }

        if (G.uart_rx_len < total_len)
            break;  /* Need more data */

        /* Validate CRC and process */
        if (phdp_validate_crc(G.uart_rx, total_len) == 0) {
            process_uart_packet(G.uart_rx, total_len);
            G.uart_rx_len -= total_len;
            memmove(G.uart_rx, G.uart_rx + total_len, G.uart_rx_len);
        } else {
            /* CRC mismatch — skip this STX, try next */
            logv("[uart] CRC mismatch, skipping byte\n");
            G.uart_rx_len--;
            memmove(G.uart_rx, G.uart_rx + 1, G.uart_rx_len);
        }
    }

    /* Flush any remaining non-PHDP bytes as console output.
     * In MONITORING state, the OS sends raw ASCII without PHDP framing. */
    if (G.uart_rx_len > 0 && G.state == PHDP_STATE_MONITORING) {
        /* Check if there's no STX in the remaining data */
        uint32_t i;
        for (i = 0; i < G.uart_rx_len; i++)
            if (G.uart_rx[i] == PHDP_STX) break;
        if (i == G.uart_rx_len) {
            /* All remaining bytes are console output */
            (void)!write(STDOUT_FILENO, G.uart_rx, G.uart_rx_len);
            log_ring_write((const char *)G.uart_rx, G.uart_rx_len);
            log_ring_broadcast((const char *)G.uart_rx, G.uart_rx_len);
            G.uart_rx_len = 0;
        }
    }
}

static int uart_read_available(void) {
    if (G.uart_fd < 0) return -1;

    uint32_t space = UART_RX_BUF_SIZE - G.uart_rx_len;
    if (space == 0) {
        /* Buffer full — discard oldest data */
        uint32_t drop = UART_RX_BUF_SIZE / 2;
        G.uart_rx_len -= drop;
        memmove(G.uart_rx, G.uart_rx + drop, G.uart_rx_len);
        space = UART_RX_BUF_SIZE - G.uart_rx_len;
    }

    ssize_t n = read(G.uart_fd, G.uart_rx + G.uart_rx_len, space);
    if (n > 0) {
        /* Per-read [uart-mon] is trace-only — at -v we'd be printing
         * a stderr line for every UART burst while the screen mirror
         * was already flowing through stdout. */
        if (G.state == PHDP_STATE_MONITORING)
            tracev("[uart-mon] read %zd bytes\n", n);
        hex_dump("[rx-raw]", G.uart_rx + G.uart_rx_len, (uint32_t)n);
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        /* EIO / device gone */
        return -1;
    }
    if (n == 0) return 0;

    G.uart_rx_len += (uint32_t)n;

    /* In MONITORING state bytes are normally raw console output, but
     * the kernel's boot-ROM disk backend issues PHDP packets
     * (REQ_OVERRIDE etc.) over the same UART after EXEC_START. We
     * split the WHOLE buffer (not just new bytes) at the first STX:
     * everything before STX is console, STX-and-after is handed to
     * uart_process_rx which knows how to handle partial packets that
     * straddle multiple read() calls. */
    if (G.state == PHDP_STATE_MONITORING) {
        uint32_t stx_off = G.uart_rx_len;
        for (uint32_t i = 0; i < G.uart_rx_len; i++) {
            if (G.uart_rx[i] == PHDP_STX) { stx_off = i; break; }
        }
        if (stx_off > 0) {
            /* Console-pass prefix (or whole buffer if no STX). */
            (void)!write(STDOUT_FILENO, G.uart_rx, stx_off);
            log_ring_write((const char *)G.uart_rx, stx_off);
            log_ring_broadcast((const char *)G.uart_rx, stx_off);
            memmove(G.uart_rx, G.uart_rx + stx_off, G.uart_rx_len - stx_off);
            G.uart_rx_len -= stx_off;
        }
        if (G.uart_rx_len > 0) {
            /* Buffer now starts with STX — try to parse packets.
             * uart_process_rx leaves partial packets in the buffer
             * for the next read(). */
            uart_process_rx();
        }
        return 0;
    }

    uart_process_rx();
    /* After EXEC_START processing, flush leftover RX buffer */
    if (G.monitoring_flush_pending && G.state == PHDP_STATE_MONITORING) {
        G.uart_rx_len = 0;
        G.monitoring_flush_pending = 0;
    }
    return 0;
}

/* ── IPC handling ───────────────────────────────────────────── */

static void ipc_remove_client(int idx) {
    close(G.clients[idx].fd);
    G.clients[idx] = G.clients[G.num_clients - 1];
    G.num_clients--;
}

static void ipc_send_status(int client_fd) {
    phdp_ipc_status_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.state = (uint8_t)G.state;
    resp.num_queued = (uint8_t)queue_count();
    if (G.streaming_active) {
        resp.bytes_sent  = G.stream.bytes_sent;
        resp.bytes_total = G.stream.file_size;
    }
    (void)write(client_fd, &resp, sizeof(resp));
}

static void ipc_handle_push(int client_fd, const phdp_ipc_req_t *req) {
    uint8_t slot = req->slot;
    uint8_t ack;

    if (slot >= PHDP_MAX_SLOTS) {
        logv("[ipc] push: invalid slot %u\n", slot);
        ack = 1;
        (void)write(client_fd, &ack, 1);
        return;
    }

    /* Verify file exists */
    if (access(req->path, R_OK) != 0) {
        logv("[ipc] push: cannot read %s: %s\n", req->path, strerror(errno));
        ack = 2;
        (void)write(client_fd, &ack, 1);
        return;
    }

    G.push_queue[slot].slot_id = slot;
    G.push_queue[slot].active = true;
    snprintf(G.push_queue[slot].path, sizeof(G.push_queue[slot].path), "%s", req->path);
    queue_save();

    ack = 0;
    (void)write(client_fd, &ack, 1);
    logv("[ipc] push slot %u <- %s\n", slot, req->path);
}

static void ipc_handle_clear(int client_fd, const phdp_ipc_req_t *req) {
    uint8_t slot = req->slot;
    if (slot == 0xFF) {
        /* Clear all */
        for (int i = 0; i < PHDP_MAX_SLOTS; i++)
            G.push_queue[i].active = false;
        logv("[ipc] cleared all slots\n");
    } else if (slot < PHDP_MAX_SLOTS) {
        G.push_queue[slot].active = false;
        logv("[ipc] cleared slot %u\n", slot);
    }
    queue_save();

    uint8_t ack_c = 0;
    (void)write(client_fd, &ack_c, 1);
}

static void ipc_handle_reset(int client_fd) {
    logv("[ipc] sending SYS_RESET\n");

    /* Abort any active stream */
    stream_abort();

    if (G.uart_fd >= 0 && G.state >= PHDP_STATE_CONNECTED) {
        uart_send_packet(PHDP_CMD_SYS_RESET, NULL, 0);
        set_state(PHDP_STATE_LISTENING);
    }

    uint8_t ack_r = 0;
    (void)write(client_fd, &ack_r, 1);
}

static void ipc_handle_wait(int client_idx) {
    if (G.state == PHDP_STATE_MONITORING) {
        /* Already executing — respond immediately. 0 = success
         * (phdp.c cmd_wait treats result==0 as EXIT_OK). */
        uint8_t ok = 0;
        (void)write(G.clients[client_idx].fd, &ok, 1);
    } else {
        /* Mark client as waiting */
        G.clients[client_idx].wait_exec = true;
    }
}

static void ipc_handle_logs(int client_idx, const phdp_ipc_req_t *req) {
    uint32_t line_count = req->arg;

    if (line_count == 0) {
        /* Continuous streaming — first dump existing logs, then mark for streaming */
        uint32_t elen = 0;
        char *existing = log_ring_read(0, &elen);
        if (existing && elen > 0) {
            (void)write(G.clients[client_idx].fd, existing, elen);
            free(existing);
        }
        G.clients[client_idx].stream_logs = true;
    } else {
        /* Return last N lines and close — client reads until EOF */
        uint32_t llen = 0;
        char *lines = log_ring_read(line_count, &llen);
        if (lines && llen > 0)
            (void)write(G.clients[client_idx].fd, lines, llen);
        free(lines);
        close(G.clients[client_idx].fd);
        G.clients[client_idx] = G.clients[G.num_clients - 1];
        G.num_clients--;
    }
}

static const char *ipc_cmd_name(uint8_t cmd) {
    switch (cmd) {
    case PHDP_IPC_STATUS: return "STATUS";
    case PHDP_IPC_PUSH:   return "PUSH";
    case PHDP_IPC_CLEAR:  return "CLEAR";
    case PHDP_IPC_RESET:  return "RESET";
    case PHDP_IPC_WAIT:   return "WAIT";
    case PHDP_IPC_LOGS:   return "LOGS";
    default:              return "?";
    }
}

static void ipc_handle_command(int client_idx) {
    phdp_ipc_req_t req;
    memset(&req, 0, sizeof(req));

    ssize_t n = read(G.clients[client_idx].fd, &req, sizeof(req));
    if (n <= 0) {
        tracev("[ipc] client %d disconnected (read=%zd)\n", client_idx, n);
        ipc_remove_client(client_idx);
        return;
    }

    tracev("[ipc] client %d: %s slot=%u arg=%u path='%.40s'\n",
           client_idx, ipc_cmd_name(req.cmd), req.slot, req.arg, req.path);

    switch (req.cmd) {
    case PHDP_IPC_STATUS:
        ipc_send_status(G.clients[client_idx].fd);
        break;
    case PHDP_IPC_PUSH:
        ipc_handle_push(G.clients[client_idx].fd, &req);
        break;
    case PHDP_IPC_CLEAR:
        ipc_handle_clear(G.clients[client_idx].fd, &req);
        break;
    case PHDP_IPC_RESET:
        ipc_handle_reset(G.clients[client_idx].fd);
        break;
    case PHDP_IPC_WAIT:
        ipc_handle_wait(client_idx);
        break;
    case PHDP_IPC_LOGS:
        ipc_handle_logs(client_idx, &req);
        break;
    default:
        logv("[ipc] unknown command %u\n", req.cmd);
        break;
    }
}

static int ipc_listen_setup(void) {
    unlink(PHDP_SOCK_PATH);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, PHDP_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 4) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    /* Allow all users to connect */
    chmod(PHDP_SOCK_PATH, 0777);

    return fd;
}

static void ipc_accept(void) {
    int fd = accept(G.listen_fd, NULL, NULL);
    if (fd < 0) return;

    if (G.num_clients >= MAX_CLIENTS) {
        const char *err = "ERR: too many clients\n";
        (void)write(fd, err, strlen(err));
        close(fd);
        tracev("[ipc] rejected client (max=%d)\n", MAX_CLIENTS);
        return;
    }

    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    memset(&G.clients[G.num_clients], 0, sizeof(ipc_client_t));
    G.clients[G.num_clients].fd = fd;
    G.num_clients++;
    tracev("[ipc] client %d accepted (total=%d)\n", G.num_clients - 1, G.num_clients);
}

/* ── Timeout handling ───────────────────────────────────────── */

static void handle_timeouts(void) {
    if (G.streaming_active && G.stream.waiting_ack) {
        int64_t elapsed = now_ms() - G.stream.last_chunk_ms;
        if (elapsed > PHDP_CHUNK_TIMEOUT_MS) {
            logv("[timeout] chunk ACK timeout after %lld ms\n", (long long)elapsed);
            stream_retry_last_chunk();
        }
    }
}

/* ── Auto-reconnect ─────────────────────────────────────────── */

static void try_uart_connect(void) {
    int fd = uart_open();
    if (fd >= 0) {
        G.uart_fd = fd;
        G.uart_rx_len = 0;
        set_state(PHDP_STATE_LISTENING);
        /* Log even at -v level so the very first thing the user sees
         * after `Starting phdpd...` is whether we got the device — the
         * #1 question when nothing else appears later. */
        logv("[uart] opened %s at %d baud\n", G.uart_path, G.baud);
        return;
    }

    /* uart_open() failed — surface why so the user can act on it.
     * This is rate-limited by the auto-reconnect throttle in the main
     * loop (every 2s), so it won't spam. */
    static int last_errno = 0;
    if (errno != last_errno) {
        logv("[uart] open %s failed: %s\n", G.uart_path, strerror(errno));
        last_errno = errno;
    }
}

/* ── Main event loop ────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [-d /dev/ttyUSB0] [-b 2000000] [-v] [-t]\n"
        "  -d PATH    UART device (default /dev/ttyUSB0)\n"
        "  -b BAUD    UART baud rate (default 2000000)\n"
        "  -v         verbose: state, packet headers, stream progress\n"
        "  -t         trace: -v + per-byte hex dumps + IPC events + timestamps\n",
        prog);
    exit(1);
}

int main(int argc, char **argv) {
    /* Defaults */
    G.uart_path = "/dev/ttyUSB0";
    G.baud      = PHDP_DEFAULT_BAUD;
    G.verbose   = false;
    G.trace     = false;
    G.uart_fd   = -1;
    G.listen_fd = -1;
    G.state     = PHDP_STATE_DISCONNECTED;

    int opt;
    while ((opt = getopt(argc, argv, "d:b:vth")) != -1) {
        switch (opt) {
        case 'd': G.uart_path = optarg; break;
        case 'b': G.baud = atoi(optarg); break;
        case 'v': G.verbose = true; break;
        case 't': G.trace = true; G.verbose = true; break;  /* trace implies verbose */
        default:  usage(argv[0]);
        }
    }

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Load persisted push queue */
    queue_load();
    logv("[init] loaded %d queued slots\n", queue_count());

    /* Set up IPC listen socket */
    G.listen_fd = ipc_listen_setup();
    if (G.listen_fd < 0) {
        fprintf(stderr, "failed to create IPC socket\n");
        return 1;
    }

    /* Initial UART connection attempt */
    try_uart_connect();

    fprintf(stderr, "phdpd: running (uart=%s baud=%d)\n", G.uart_path, G.baud);

    int64_t last_reconnect_attempt = 0;

    while (!g_quit) {
        /* Build poll set: UART + listen socket + client sockets */
        struct pollfd fds[2 + MAX_CLIENTS];
        int nfds = 0;

        int uart_poll_idx = -1;
        if (G.uart_fd >= 0) {
            uart_poll_idx = nfds;
            fds[nfds].fd = G.uart_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        int listen_poll_idx = nfds;
        fds[nfds].fd = G.listen_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        int client_base_idx = nfds;
        for (int i = 0; i < G.num_clients; i++) {
            fds[nfds].fd = G.clients[i].fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        /* Compute timeout */
        int poll_timeout;
        if (G.state == PHDP_STATE_DISCONNECTED)
            poll_timeout = 1000; /* reconnect interval */
        else if (G.streaming_active && G.stream.waiting_ack)
            poll_timeout = 100;  /* check chunk timeout frequently */
        else
            poll_timeout = 500;

        int ret = poll(fds, (nfds_t)nfds, poll_timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        /* Handle UART data */
        if (uart_poll_idx >= 0) {
            if (fds[uart_poll_idx].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                logv("[uart] device error/disconnect\n");
                uart_close();
                stream_abort();
                set_state(PHDP_STATE_DISCONNECTED);
            } else if (fds[uart_poll_idx].revents & POLLIN) {
                if (uart_read_available() < 0) {
                    logv("[uart] read error, disconnecting\n");
                    uart_close();
                    stream_abort();
                    set_state(PHDP_STATE_DISCONNECTED);
                }
            }
        }

        /* Accept new IPC clients */
        if (fds[listen_poll_idx].revents & POLLIN) {
            ipc_accept();
        }

        /* Handle IPC client data — iterate in reverse so removal is safe */
        for (int i = G.num_clients - 1; i >= 0; i--) {
            int pidx = client_base_idx + i;
            if (pidx < nfds && fds[pidx].revents & (POLLIN | POLLERR | POLLHUP)) {
                if (fds[pidx].revents & (POLLERR | POLLHUP)) {
                    ipc_remove_client(i);
                } else {
                    ipc_handle_command(i);
                }
            }
        }

        /* Timeouts */
        handle_timeouts();

        /* Auto-reconnect */
        if (G.state == PHDP_STATE_DISCONNECTED) {
            int64_t t = now_ms();
            if (t - last_reconnect_attempt > 1000) {
                last_reconnect_attempt = t;
                try_uart_connect();
            }
        }

        /* Drive streaming if we have data to send and aren't waiting for an ACK */
        if (G.streaming_active && !G.stream.waiting_ack) {
            stream_send_next_chunk();
        }
    }

    /* Cleanup */
    fprintf(stderr, "phdpd: shutting down\n");
    uart_close();
    for (int i = 0; i < G.num_clients; i++)
        close(G.clients[i].fd);
    close(G.listen_fd);
    unlink(PHDP_SOCK_PATH);

    return 0;
}
