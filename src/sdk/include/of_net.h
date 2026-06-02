//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_net.h -- Host/client networking API for openfpgaOS
 *
 * Byte-stream communication between devices.
 * Pocket: link cable (1 host, 1 client).
 * MiSTer: user port. PC: sockets.
 *
 * Usage (client):
 *   of_net_join();
 *   of_net_send(data, len);
 *   of_net_recv(buf, sizeof(buf));
 *
 * Usage (host):
 *   of_net_host_start();
 *   of_net_broadcast(data, len);
 *   of_net_recv_from(0, buf, sizeof(buf));
 */

#ifndef OF_NET_H
#define OF_NET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define OF_NET_DISCONNECTED  0
#define OF_NET_HOSTING       1
#define OF_NET_JOINED        2

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

/* Per the kernel convention in syscall_dispatch (kernel/syscall.c),
 * vendor calls returning a negative int land in sbiret.error and
 * sbiret.value is zeroed; non-negative results land in sbiret.value.
 * The wrappers must surface the error first or callers see 0 instead
 * of -1 on failure (e.g. of_net_send when DISCONNECTED). */
static inline int __of_net_ret(struct of_sbiret r) {
    return r.error ? (int)r.error : (int)r.value;
}

static inline int of_net_host_start(void) {
    return __of_net_ret(of_ecall0(OF_EID_NET, OF_NET_FID_HOST_START));
}
static inline int of_net_join(void) {
    return __of_net_ret(of_ecall0(OF_EID_NET, OF_NET_FID_JOIN));
}
static inline void of_net_stop(void) {
    of_ecall0(OF_EID_NET, OF_NET_FID_STOP);
}
static inline int of_net_status(void) {
    return __of_net_ret(of_ecall0(OF_EID_NET, OF_NET_FID_STATUS));
}
static inline int of_net_client_count(void) {
    return __of_net_ret(of_ecall0(OF_EID_NET, OF_NET_FID_CLIENT_COUNT));
}
static inline int of_net_send_to(int client, const void *data, size_t len) {
    return __of_net_ret(of_ecall3(OF_EID_NET, OF_NET_FID_SEND_TO,
                                  client, (long)data, len));
}
static inline int of_net_recv_from(int client, void *data, size_t len) {
    return __of_net_ret(of_ecall3(OF_EID_NET, OF_NET_FID_RECV_FROM,
                                  client, (long)data, len));
}
static inline int of_net_broadcast(const void *data, size_t len) {
    return __of_net_ret(of_ecall2(OF_EID_NET, OF_NET_FID_BROADCAST,
                                  (long)data, len));
}
static inline int of_net_send(const void *data, size_t len) {
    return __of_net_ret(of_ecall2(OF_EID_NET, OF_NET_FID_SEND,
                                  (long)data, len));
}
static inline int of_net_recv(void *data, size_t len) {
    return __of_net_ret(of_ecall2(OF_EID_NET, OF_NET_FID_RECV,
                                  (long)data, len));
}
static inline int of_net_poll(void) {
    return __of_net_ret(of_ecall0(OF_EID_NET, OF_NET_FID_POLL));
}

#else /* OF_PC */

static inline int of_net_host_start(void) { return -1; }
static inline int of_net_join(void) { return -1; }
static inline void of_net_stop(void) {}
static inline int of_net_status(void) { return OF_NET_DISCONNECTED; }
static inline int of_net_client_count(void) { return 0; }
static inline int of_net_send_to(int c, const void *d, size_t l) {
    (void)c; (void)d; (void)l; return -1;
}
static inline int of_net_recv_from(int c, void *d, size_t l) {
    (void)c; (void)d; (void)l; return -1;
}
static inline int of_net_broadcast(const void *d, size_t l) {
    (void)d; (void)l; return -1;
}
static inline int of_net_send(const void *d, size_t l) {
    (void)d; (void)l; return -1;
}
static inline int of_net_recv(void *d, size_t l) {
    (void)d; (void)l; return -1;
}
static inline int of_net_poll(void) { return 0; }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_NET_H */
