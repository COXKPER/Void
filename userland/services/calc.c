/* VoidOS — Phase 9 user-space service prototype: calculator
 *
 * The minimal proof that a user-space service can be discovered through the
 * kernel service registry and serve requests over plain IPC.  This process:
 *   1. creates an IPC endpoint (its service "inbox")
 *   2. registers it under the name "calc"
 *   3. loops: receives a request, computes, replies back to the client.
 *
 * The request carries the client's own endpoint address (pid + handle) so
 * the service can reply directly back.  The tag echoes which client-
 * conversation the reply belongs to.  This is the entire service; the
 * protocol is flat bytes + one op.  Anything bigger would be overbuilding.
 *
 * Wire format (calc_proto.h): requests and responses are flat byte arrays.
 */
#include <void.h>
#include <ipc.h>
#include <svc.h>
#include "calc_proto.h"

static void say(const char *s) { sys_write(1, s, 16); }

static long handle_request(const uint8_t *req, uint8_t *reply) {
    uint32_t op = *(uint32_t *)(req + CALC_REQ_OP);
    int32_t  a  = *(int32_t  *)(req + CALC_REQ_A);
    int32_t  b  = *(int32_t  *)(req + CALC_REQ_B);

    if (op == CALC_OP_ADD) { *(int32_t *)(reply + CALC_RESP_RESULT) = a + b; *(uint32_t *)(reply + CALC_RESP_STATUS) = 0; return CALC_RESP_LEN; }
    if (op == CALC_OP_SUB) { *(int32_t *)(reply + CALC_RESP_RESULT) = a - b; *(uint32_t *)(reply + CALC_RESP_STATUS) = 0; return CALC_RESP_LEN; }

    return -1;   /* unknown op */
}

int main(void) {
    say("[calc] up\n");

    /* 1. create the service endpoint. */
    long server_h = ipc_endpoint_create();
    if (server_h < 0) {
        say("[calc] endpoint create FAIL\n");
        sys_exit(1);
    }

    /* 2. register it under the name "calc". */
    long reg = sr_register("calc", (int)server_h);
    if (reg < 0) {
        say("[calc] register FAIL\n");
        sys_exit(2);
    }

    say("[calc] registered\n");

    /* 3. serve forever. */
    uint8_t buf[512];
    for (;;) {
        /* Receive the next request (poll, then yield to make progress). */
        uint32_t tag = 0;
        long n = ipc_recv((int)server_h, &tag, buf, sizeof(buf));
        if (n == 0) { sys_sched_yield(); continue; }
        if (n < 0)  { say("[calc] recv err\n"); break; }

        if (n < CALC_REQ_LEN) { say("[calc] short req\n"); continue; }

        /* Wrap the client's endpoint into a real handle so we can reply. */
        uint32_t client_pid     = *(uint32_t *)(buf + CALC_REQ_PID);
        uint32_t client_endpoint = *(uint32_t *)(buf + CALC_REQ_EP);
        long reply_h = ipc_connect(client_pid, client_endpoint);
        if (reply_h < 0) { say("[calc] connect-cli FAIL\n"); continue; }

        /* Compute + reply (tag echoes the request's conversation). */
        uint8_t reply[64];
        long rlen = handle_request(buf, reply);
        if (rlen < 0) { say("[calc] bad op\n"); continue; }

        long s = ipc_send((int)reply_h, tag, reply, (uint32_t)rlen);
        if (s < 0) { say("[calc] reply FAIL\n"); }
    }

    return 0;
}