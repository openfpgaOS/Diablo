//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * phdp.c — PHDP CLI client
 *
 * Thin client that sends commands to the phdpd daemon via Unix domain socket.
 * See PHDP spec Section 6 for command descriptions.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "phdp_proto.h"

/* Exit codes */
#define EXIT_OK      0
#define EXIT_ERR     1
#define EXIT_TIMEOUT 2

/* ── Helpers ─────────────────────────────────────────────────── */

static const char *state_name(uint8_t state)
{
    switch (state) {
    case PHDP_STATE_DISCONNECTED: return "DISCONNECTED";
    case PHDP_STATE_LISTENING:    return "LISTENING";
    case PHDP_STATE_CONNECTED:    return "CONNECTED";
    case PHDP_STATE_READY:        return "READY";
    case PHDP_STATE_STREAMING:    return "STREAMING";
    case PHDP_STATE_MONITORING:   return "MONITORING";
    default:                      return "UNKNOWN";
    }
}

/*
 * Connect to the daemon's Unix socket.
 * Returns the fd on success, or -1 on failure (prints error).
 */
static int daemon_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "error: socket(): %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, PHDP_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno == ECONNREFUSED || errno == ENOENT)
            fprintf(stderr, "error: daemon not running (cannot connect to %s)\n",
                    PHDP_SOCK_PATH);
        else
            fprintf(stderr, "error: connect(%s): %s\n",
                    PHDP_SOCK_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * Send the full IPC request. Returns 0 on success, -1 on failure.
 */
static int send_request(int fd, const phdp_ipc_req_t *req)
{
    const uint8_t *p = (const uint8_t *)req;
    size_t remaining = sizeof(*req);

    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "error: write(): %s\n", strerror(errno));
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/*
 * Read exactly `len` bytes from fd. Returns 0 on success, -1 on error/EOF.
 */
static int recv_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "error: read(): %s\n", strerror(errno));
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "error: daemon closed connection unexpectedly\n");
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* ── Subcommands ─────────────────────────────────────────────── */

static int cmd_status(int fd)
{
    phdp_ipc_req_t req;
    memset(&req, 0, sizeof(req));
    req.cmd = PHDP_IPC_STATUS;

    if (send_request(fd, &req) < 0)
        return EXIT_ERR;

    phdp_ipc_status_t st;
    if (recv_exact(fd, &st, sizeof(st)) < 0)
        return EXIT_ERR;

    printf("state:    %s (%u)\n", state_name(st.state), st.state);
    printf("queued:   %u slot(s)\n", st.num_queued);

    if (st.state == PHDP_STATE_STREAMING) {
        double pct = st.bytes_total > 0
            ? (double)st.bytes_sent / (double)st.bytes_total * 100.0
            : 0.0;
        printf("progress: %u / %u bytes (%.1f%%)\n",
               st.bytes_sent, st.bytes_total, pct);
    }

    return EXIT_OK;
}

static int cmd_push(int fd, int slot, const char *filepath)
{
    /* Resolve to absolute path so daemon doesn't depend on client cwd */
    char abspath[PATH_MAX];
    if (realpath(filepath, abspath) == NULL) {
        fprintf(stderr, "error: %s: %s\n", filepath, strerror(errno));
        return EXIT_ERR;
    }

    if (strlen(abspath) >= sizeof(((phdp_ipc_req_t *)0)->path)) {
        fprintf(stderr, "error: path too long (max %zu bytes)\n",
                sizeof(((phdp_ipc_req_t *)0)->path) - 1);
        return EXIT_ERR;
    }

    phdp_ipc_req_t req;
    memset(&req, 0, sizeof(req));
    req.cmd  = PHDP_IPC_PUSH;
    req.slot = (uint8_t)slot;
    strncpy(req.path, abspath, sizeof(req.path) - 1);

    if (send_request(fd, &req) < 0)
        return EXIT_ERR;

    /* Read 1-byte ack: 0 = ok, nonzero = error */
    uint8_t ack;
    if (recv_exact(fd, &ack, sizeof(ack)) < 0)
        return EXIT_ERR;

    if (ack != 0) {
        fprintf(stderr, "error: daemon rejected push (code %u)\n", ack);
        return EXIT_ERR;
    }

    printf("queued slot %d: %s\n", slot, abspath);
    return EXIT_OK;
}

static int cmd_clear(int fd, int slot)
{
    phdp_ipc_req_t req;
    memset(&req, 0, sizeof(req));
    req.cmd  = PHDP_IPC_CLEAR;
    req.slot = (uint8_t)slot;  /* 0xFF = clear all */

    if (send_request(fd, &req) < 0)
        return EXIT_ERR;

    uint8_t ack;
    if (recv_exact(fd, &ack, sizeof(ack)) < 0)
        return EXIT_ERR;

    if (ack != 0) {
        fprintf(stderr, "error: daemon rejected clear (code %u)\n", ack);
        return EXIT_ERR;
    }

    if (slot == 0xFF)
        printf("cleared all slots\n");
    else
        printf("cleared slot %d\n", slot);

    return EXIT_OK;
}

static int cmd_reset(int fd)
{
    phdp_ipc_req_t req;
    memset(&req, 0, sizeof(req));
    req.cmd = PHDP_IPC_RESET;

    if (send_request(fd, &req) < 0)
        return EXIT_ERR;

    uint8_t ack;
    if (recv_exact(fd, &ack, sizeof(ack)) < 0)
        return EXIT_ERR;

    if (ack != 0) {
        fprintf(stderr, "error: daemon rejected reset (code %u)\n", ack);
        return EXIT_ERR;
    }

    printf("reset sent\n");
    return EXIT_OK;
}

static int cmd_wait(int fd)
{
    phdp_ipc_req_t req;
    memset(&req, 0, sizeof(req));
    req.cmd = PHDP_IPC_WAIT;

    if (send_request(fd, &req) < 0)
        return EXIT_ERR;

    /*
     * Daemon blocks the response until EVT_EXEC_START arrives,
     * or returns an error/timeout code.
     */
    uint8_t result;
    if (recv_exact(fd, &result, sizeof(result)) < 0)
        return EXIT_ERR;

    switch (result) {
    case 0:
        printf("execution started\n");
        return EXIT_OK;
    case 2:
        fprintf(stderr, "error: timed out waiting for execution\n");
        return EXIT_TIMEOUT;
    default:
        fprintf(stderr, "error: wait failed (code %u)\n", result);
        return EXIT_ERR;
    }
}

static int cmd_logs(int fd, uint32_t last_n)
{
    phdp_ipc_req_t req;
    memset(&req, 0, sizeof(req));
    req.cmd = PHDP_IPC_LOGS;
    req.arg = last_n;  /* 0 = stream continuously */

    if (send_request(fd, &req) < 0)
        return EXIT_ERR;

    /*
     * Read log lines from daemon.
     * Protocol: daemon sends lines as raw text terminated by EOF (close).
     */
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "error: read(): %s\n", strerror(errno));
            return EXIT_ERR;
        }
        if (n == 0)
            break;  /* daemon closed connection — end of output */

        /* Write directly to stdout */
        size_t written = 0;
        while (written < (size_t)n) {
            ssize_t w = write(STDOUT_FILENO, buf + written, (size_t)n - written);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                return EXIT_ERR;
            }
            written += (size_t)w;
        }
    }

    return EXIT_OK;
}

/* ── Usage ───────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr,
        "usage: phdp <command> [options]\n"
        "\n"
        "commands:\n"
        "  status              show daemon state, queued slots, transfer progress\n"
        "  push --slot N file  queue a binary for data slot N\n"
        "  clear [--slot N]    clear override for slot N (omit --slot to clear all)\n"
        "  reset               send CMD_SYS_RESET to reboot the Pocket core\n"
        "  wait                block until execution starts on the Pocket\n"
        "  logs [--last N]     print console logs (--last N for last N lines)\n"
        "\n"
        "exit codes: 0 = success, 1 = error, 2 = timeout\n"
    );
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return EXIT_ERR;
    }

    const char *subcmd = argv[1];

    /* Shift argv so getopt in subcommands sees subcommand args */
    argc--;
    argv++;

    int fd = -1;
    int rc = EXIT_ERR;

    /* ── status ── */
    if (strcmp(subcmd, "status") == 0) {
        if (argc != 1) {
            fprintf(stderr, "usage: phdp status\n");
            return EXIT_ERR;
        }
        fd = daemon_connect();
        if (fd < 0)
            return EXIT_ERR;
        rc = cmd_status(fd);
    }

    /* ── push ── */
    else if (strcmp(subcmd, "push") == 0) {
        int slot = -1;
        const char *filepath = NULL;

        static struct option long_opts[] = {
            { "slot", required_argument, NULL, 's' },
            { NULL, 0, NULL, 0 }
        };

        optind = 1;
        int opt;
        while ((opt = getopt_long(argc, argv, "s:", long_opts, NULL)) != -1) {
            switch (opt) {
            case 's': {
                char *end;
                long val = strtol(optarg, &end, 10);
                if (*end != '\0' || val < 0 || val >= PHDP_MAX_SLOTS) {
                    fprintf(stderr, "error: invalid slot number '%s' (0-%d)\n",
                            optarg, PHDP_MAX_SLOTS - 1);
                    return EXIT_ERR;
                }
                slot = (int)val;
                break;
            }
            default:
                fprintf(stderr, "usage: phdp push --slot N filepath\n");
                return EXIT_ERR;
            }
        }

        if (slot < 0 || optind >= argc) {
            fprintf(stderr, "usage: phdp push --slot N filepath\n");
            return EXIT_ERR;
        }
        filepath = argv[optind];

        /* Verify file exists before bothering the daemon */
        if (access(filepath, R_OK) != 0) {
            fprintf(stderr, "error: %s: %s\n", filepath, strerror(errno));
            return EXIT_ERR;
        }

        fd = daemon_connect();
        if (fd < 0)
            return EXIT_ERR;
        rc = cmd_push(fd, slot, filepath);
    }

    /* ── clear ── */
    else if (strcmp(subcmd, "clear") == 0) {
        int slot = 0xFF;  /* default: clear all */

        static struct option long_opts[] = {
            { "slot", required_argument, NULL, 's' },
            { NULL, 0, NULL, 0 }
        };

        optind = 1;
        int opt;
        while ((opt = getopt_long(argc, argv, "s:", long_opts, NULL)) != -1) {
            switch (opt) {
            case 's': {
                char *end;
                long val = strtol(optarg, &end, 10);
                if (*end != '\0' || val < 0 || val >= PHDP_MAX_SLOTS) {
                    fprintf(stderr, "error: invalid slot number '%s' (0-%d)\n",
                            optarg, PHDP_MAX_SLOTS - 1);
                    return EXIT_ERR;
                }
                slot = (int)val;
                break;
            }
            default:
                fprintf(stderr, "usage: phdp clear [--slot N]\n");
                return EXIT_ERR;
            }
        }

        fd = daemon_connect();
        if (fd < 0)
            return EXIT_ERR;
        rc = cmd_clear(fd, slot);
    }

    /* ── reset ── */
    else if (strcmp(subcmd, "reset") == 0) {
        if (argc != 1) {
            fprintf(stderr, "usage: phdp reset\n");
            return EXIT_ERR;
        }
        fd = daemon_connect();
        if (fd < 0)
            return EXIT_ERR;
        rc = cmd_reset(fd);
    }

    /* ── wait ── */
    else if (strcmp(subcmd, "wait") == 0) {
        if (argc != 1) {
            fprintf(stderr, "usage: phdp wait\n");
            return EXIT_ERR;
        }
        fd = daemon_connect();
        if (fd < 0)
            return EXIT_ERR;
        rc = cmd_wait(fd);
    }

    /* ── logs ── */
    else if (strcmp(subcmd, "logs") == 0) {
        uint32_t last_n = 0;  /* 0 = stream continuously */

        static struct option long_opts[] = {
            { "last", required_argument, NULL, 'l' },
            { NULL, 0, NULL, 0 }
        };

        optind = 1;
        int opt;
        while ((opt = getopt_long(argc, argv, "l:", long_opts, NULL)) != -1) {
            switch (opt) {
            case 'l': {
                char *end;
                unsigned long val = strtoul(optarg, &end, 10);
                if (*end != '\0' || val == 0 || val > UINT32_MAX) {
                    fprintf(stderr, "error: invalid line count '%s'\n", optarg);
                    return EXIT_ERR;
                }
                last_n = (uint32_t)val;
                break;
            }
            default:
                fprintf(stderr, "usage: phdp logs [--last N]\n");
                return EXIT_ERR;
            }
        }

        fd = daemon_connect();
        if (fd < 0)
            return EXIT_ERR;
        rc = cmd_logs(fd, last_n);
    }

    /* ── help / unknown ── */
    else if (strcmp(subcmd, "help") == 0 || strcmp(subcmd, "--help") == 0 ||
             strcmp(subcmd, "-h") == 0) {
        usage();
        rc = EXIT_OK;
    }
    else {
        fprintf(stderr, "error: unknown command '%s'\n\n", subcmd);
        usage();
        rc = EXIT_ERR;
    }

    if (fd >= 0)
        close(fd);

    return rc;
}
