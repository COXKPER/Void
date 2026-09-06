/* VoidOS — Phase 9 service-registry + user-service test client
 *
 * Exercises the complete user-space service lifecycle (Step 5):
 *   T1  lookup("calc") → a handle to the calc service's endpoint
 *   T2  send a request (add), receive the reply (response framing works)
 *   T3  send a request (sub), receive the reply
 *   T4  multiple requests in sequence (tag echo + FIFO)
 *   T5  lookup of a missing service fails (-ENOENT)
 *   T6  invalid-name lookup (empty string) fails (-EINVAL)
 *
 * The flow proves: client → lookup("calc") → IPC connection → calc service
 * → response.  The calc service must be running under the name "calc".
 */
#include <void.h>
#include <ipc.h>
#include <svc.h>
#include <string.h>
#include "calc_proto.h"

static int failures;

static void say(const char *s) { sys_write(1, s, 16); }

static void print_dec(long v) {
    char buf[16]; int i = 0;
    if (v == 0) buf[i++] = '0';
    if (v < 0) { sys_write(1, "-", 1); v = -v; }
    while (v > 0 && i < 15) { buf[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i > 0) sys_write(1, &buf[--i], 1);
}

static void check(const char *name, int len, long got, long want, int pass) {
    if (!pass) failures++;
    sys_write(1, "[srv] ", 6);
    sys_write(1, name, len);
    sys_write(1, ": ", 2);
    sys_write(1, pass ? "PASS" : "FAIL", 4);
    sys_write(1, " (got ", 6);
    print_dec(got);
    sys_write(1, " want ", 6);
    print_dec(want);
    sys_write(1, ")\n", 2);
}

/* Build a request: flat [pid][endpoint][op][a][b]. */
static void build_req(uint8_t *out, uint32_t ep, uint32_t op, int32_t a, int32_t b) {
    memset(out, 0, CALC_REQ_LEN);
    *(uint32_t *)(out + CALC_REQ_PID) = (uint32_t)(uint64_t)sys_getpid();
    *(uint32_t *)(out + CALC_REQ_EP)  = ep;
    *(uint32_t *)(out + CALC_REQ_OP)  = op;
    *(int32_t  *)(out + CALC_REQ_A)   = a;
    *(int32_t  *)(out + CALC_REQ_B)   = b;
}

/* Send a request to the calc service and wait for the reply.  Returns the
 * result word, or the sentinel `bad` if no reply arrived. */
static long calc_request(long srv, uint32_t op, int32_t a, int32_t b, long bad) {
    long mine = ipc_endpoint_create();
    if (mine < 0) return bad;

    uint8_t req[CALC_REQ_LEN];
    build_req(req, (uint32_t)mine, op, a, b);

    long s = ipc_send((int)srv, op, req, sizeof(req));
    if (s < 0) return bad;

    uint8_t reply[64];
    long got = bad;
    for (int spin = 0; spin < 400; spin++) {
        uint32_t tag = 0;
        long n = ipc_recv((int)mine, &tag, reply, sizeof(reply));
        if (n > 0) { got = *(int32_t *)(reply + CALC_RESP_RESULT); break; }
        sys_sched_yield();
    }
    return got;
}

int main(void) {
    say("[srvtest] client up\n");

    /* T1: discover the calc service.  The service registers asynchronously
     * at boot, so retry briefly — a client that starts before the service
     * is up retries until discovery succeeds (mirrors a client waiting for
     * a background service to come online). */
    long srv = -2;
    for (int i = 0; i < 400 && srv < 0; i++) {
        srv = sr_lookup("calc");
        if (srv < 0) sys_sched_yield();
    }
    check("lookup calc", 11, srv, 1, srv >= 0);

    if (srv >= 0) {
        /* T2: add 2 + 3 → 5. */
        long a2 = calc_request(srv, CALC_OP_ADD, 2, 3, -999);
        check("reply add", 9, a2, 5, a2 == 5);

        /* T3: sub 7 − 2 → 5. */
        long a3 = calc_request(srv, CALC_OP_SUB, 7, 2, -999);
        check("reply sub", 9, a3, 5, a3 == 5);

        /* T4: multiple sequential requests (FIFO + tag echo). */
        long m1 = calc_request(srv, CALC_OP_ADD, 10, 32, -999);   /* 42 */
        long m2 = calc_request(srv, CALC_OP_ADD, 1, 2, -999);     /* 3  */
        check("multi a1", 8, m1, 42, m1 == 42);
        check("multi a2", 8, m2, 3,  m2 == 3);
    }

    /* T5: missing service → -ENOENT. */
    long miss = sr_lookup("nosuch");
    check("lookup missing", 14, miss, -2, miss == -2);

    /* T6: invalid name → -EINVAL. */
    long badname = sr_lookup("");
    check("lookup empty", 12, badname, -22, badname == -22);

    say("\r\n[srvtest] Done: ");
    print_dec(failures);
    say(" fail(s)\r\n");

    return (failures == 0) ? 0 : 1;
}