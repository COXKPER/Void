/* VoidOS — calc service wire protocol
 *
 * Messages are flat byte arrays: the kernel's ipc_send/recv move exactly
 * data_len bytes with no framing, so the protocol is just offsets into the
 * first bytes of the payload.  Each message fits the 496-byte data plane.
 */
#ifndef CALC_PROTO_H
#define CALC_PROTO_H 1

#define CALC_OP_ADD 1
#define CALC_OP_SUB 2

/* Request (sent client → service): [u32 client_pid][u32 client_endpoint][u32 op][i32 a][i32 b] */
#define CALC_REQ_LEN    20
#define CALC_REQ_PID    0      /* client pid (the requester)   */
#define CALC_REQ_EP     4      /* client's own endpoint (the reply target) */
#define CALC_REQ_OP     8
#define CALC_REQ_A      12
#define CALC_REQ_B      16

/* Response (sent service → client): [i32 result][u32 status] */
#define CALC_RESP_LEN   8
#define CALC_RESP_RESULT 0
#define CALC_RESP_STATUS 4

#endif /* CALC_PROTO_H */