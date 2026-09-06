/* VoidOS — Phase 9 lifecycle test: service exit → registry cleanup
 *
 * Spawns a short-lived "flicker" service that:
 *   1. creates an endpoint, registers "flicker", prints its pid
 *   2. loops receiving requests until told to stop (or N requests)
 *   3. exits.
 *
 * The test process (parent) then:
 *   A. looks up "flicker" while it is up → must succeed
 *   B. sends it a "KILL" request
 *   C. waits for the service to exit (reap its pid)
 *   D. looks up "flicker" again → must fail with -ENOENT (registry cleaned)
 *
 * This proves the full Step-5 lifecycle: service starts → registers → client
 * discovers → request → reply → service exits → registry cleans → lookup
 * fails safely.  Duplicate-registration and wrong-owner cleanup are handled
 * by the same svc_cleanup_owner path.
 */
#include <void.h>
#include <ipc.h>
#include <svc.h>
#include <string.h>

#define OP_ECHO_PID 1
#define OP_KILL     2

static int failures;
static void say(const char *s) { sys_write(1, s, 16); }
static void print_dec(long v) {
    char buf[16]; int i = 0;
    if (v == 0) buf[i++] = '0';
    if (v < 0) { sys_write(1, "-", 1); v = -v; }
    while (v > 0 && i < 15) { buf[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i > 0) sys_write(1, &buf[--i], 1);
}

/* ── the short-lived service (child) ──────────────────────────────────── */
static void run_service(void) {
    long ep = ipc_endpoint_create();
    if (ep < 0) sys_exit(1);
    if (sr_register("flicker", (int)ep) < 0) sys_exit(2);

    uint8_t buf[512];
    for (;;) {
        uint32_t tag = 0;
        long n = ipc_recv((int)ep, &tag, buf, sizeof(buf));
        if (n == 0) { sys_sched_yield(); continue; }
        if (n < 0)  sys_exit(3);
        if (n < 12) sys_exit(4);

        uint32_t op = *(uint32_t *)(buf + 8);
        uint32_t client_pid     = *(uint32_t *)(buf + 0);
        uint32_t client_endpoint = *(uint32_t *)(buf + 4);

        long reply_h = ipc_connect(client_pid, client_endpoint);
        uint8_t out[64];
        if (op == OP_ECHO_PID) {
            *(int32_t *)&out[0] = (int32_t)sys_getpid();
            *(uint32_t *)&out[4] = 0;
            ipc_send((int)reply_h, tag, out, 8);
        } else if (op == OP_KILL) {
            sys_exit(0);     /* service exits; registry cleans on exit */
        }
    }
}

/* parent: drive the lifecycle. */
static void drive(void) {
    /* A: lookup while the service is up. */
    long srv = -2;
    for (int i = 0; i < 400 && srv < 0; i++) {
        srv = sr_lookup("flicker");
        if (srv < 0) sys_sched_yield();
    }
    int up_ok = (srv >= 0);
    if (!up_ok) failures++;
    say("[lifetest] lookup while up: ");
    say(up_ok ? "PASS\n" : "FAIL\n");

    /* B: ask the service for its pid (round-trip). */
    long mine_ep = ipc_endpoint_create();
    uint8_t req[12];
    memset(req, 0, sizeof(req));
    *(uint32_t *)(req + 0) = (uint32_t)(uint64_t)sys_getpid();
    *(uint32_t *)(req + 4) = (uint32_t)(uint64_t)mine_ep;
    *(uint32_t *)(req + 8) = OP_ECHO_PID;
    long sent = ipc_send((int)srv, 0, req, 12);

    long svc_pid = -1;
    if (sent == 0) {
        uint8_t reply[64];
        for (int i = 0; i < 400 && svc_pid < 0; i++) {
            uint32_t tag = 0;
            long n = ipc_recv((int)mine_ep, &tag, reply, sizeof(reply));
            if (n > 0) svc_pid = *(int32_t *)&reply[0];
            else  sys_sched_yield();
        }
    }
    int roundtrip_ok = (svc_pid > 0);
    if (!roundtrip_ok) failures++;
    say("[lifetest] roundtrip got pid: ");
    if (roundtrip_ok) { print_dec(svc_pid); say("\n"); }
    else say("FAIL\n");

    /* C: kill the service. */
    memset(req, 0, sizeof(req));
    *(uint32_t *)(req + 0) = (uint32_t)(uint64_t)sys_getpid();
    *(uint32_t *)(req + 4) = (uint32_t)(uint64_t)mine_ep;
    *(uint32_t *)(req + 8) = OP_KILL;
    ipc_send((int)srv, 0, req, 12);

    /* wait for it to exit.  The parent spawned it, so we can reap it. */
    long killed_ok = 0;
    for (int i = 0; i < 200; i++) {
        long st = 0;
        long r = sys_wait4(svc_pid, &st, 0);
        if (r == svc_pid) { killed_ok = (st == 0); break; }
        sys_sched_yield();
    }
    if (!killed_ok) failures++;
    say("[lifetest] service exited cleanly: ");
    say(killed_ok ? "PASS\n" : "FAIL\n");

    /* D: after exit, the registry must have forgotten "flicker". */
    long gone = sr_lookup("flicker");
    int gone_ok = (gone == -2);   /* -ENOENT */
    if (!gone_ok) failures++;
    say("[lifetest] lookup after exit -ENOENT: ");
    say(gone_ok ? "PASS\n" : "FAIL\n");

    say("[lifetest] Done: ");
    print_dec(failures);
    say(" fail(s)\n");
}

int main(void) {
    say("[lifetest] up\n");
    long c = sys_fork();
    if (c < 0) return 1;
    if (c == 0) {
        run_service();   /* child never returns */
        sys_exit(9);
    }
    drive();             /* parent watches the lifecycle */
    return (failures == 0) ? 0 : 1;
}