//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"

void test_net(void) {
    section_start("Net");

    /* No cable connected — should report disconnected */
    ASSERT_EQ("status", of_net_status(), OF_NET_DISCONNECTED);

    /* Host start/stop should not crash */
    of_net_host_start();
    ASSERT_EQ("hosting", of_net_status(), OF_NET_HOSTING);
    int clients = of_net_client_count();
    ASSERT("clients 0-1", clients >= 0 && clients <= 1);
    of_net_stop();
    ASSERT_EQ("stopped", of_net_status(), OF_NET_DISCONNECTED);

    /* Join/stop should not crash */
    of_net_join();
    ASSERT_EQ("joined", of_net_status(), OF_NET_JOINED);
    of_net_stop();

    /* Poll with no connection returns 0 */
    ASSERT_EQ("poll", of_net_poll(), 0);

    /* Send/recv with no connection return error */
    uint8_t buf[4] = {1, 2, 3, 4};
    ASSERT("send fail", of_net_send(buf, 4) < 0);
    ASSERT("recv zero", of_net_recv(buf, 4) <= 0);

    section_end();
}
