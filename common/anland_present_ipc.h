#ifndef ANLAND_PRESENT_IPC_H
#define ANLAND_PRESENT_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anland_present_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Send one complete framed message. FDs are transferred with SCM_RIGHTS and
 * remain owned by the caller. The peer receives duplicates. */
int anland_present_ipc_send(int fd,
                            const anland_present_msg_header_t *header,
                            const void *payload,
                            size_t payload_size,
                            const int *fds,
                            size_t fd_count);

/* Receive one complete framed message. The payload is copied into caller
 * storage. Received FDs are owned by the caller and must be closed. */
int anland_present_ipc_recv(int fd,
                            anland_present_msg_header_t *header,
                            void *payload,
                            size_t payload_capacity,
                            size_t *payload_size,
                            int *fds,
                            size_t fd_capacity,
                            size_t *fd_count);

void anland_present_ipc_close_fds(int *fds, size_t fd_count);

/* ---- incremental receive -------------------------------------------------
 *
 * anland_present_ipc_recv() above is for synchronous callers on a socket that
 * is known to be carrying a complete message. A pump() loop cannot use it: it
 * blocks inside MSG_WAITALL until the peer finishes the message, so a peer that
 * sends half a header and then stalls would block the caller even with a zero
 * timeout.
 *
 * This reader keeps the framing state across calls (header bytes, payload
 * bytes, and the FDs that arrived with the header), so each pump() makes
 * progress and returns as soon as its budget is spent. A message is only
 * reported once every byte has arrived; nothing partial is ever handed out.
 */
typedef struct anland_present_ipc_reader {
    anland_present_msg_header_t header;
    size_t header_bytes;   /* bytes of `header` already read */
    size_t payload_size;   /* payload length announced by the header */
    size_t payload_bytes;  /* payload bytes already stored in the caller buffer */
    int fds[ANLAND_PRESENT_MAX_FDS];
    size_t fd_count;
    bool have_header;      /* the header arrived and passed validation */
} anland_present_ipc_reader_t;

typedef enum anland_present_ipc_status {
    ANLAND_PRESENT_IPC_ERROR = -1,      /* protocol error, or the socket died */
    ANLAND_PRESENT_IPC_INCOMPLETE = 0,  /* budget spent before a whole message */
    ANLAND_PRESENT_IPC_MESSAGE = 1,     /* one complete message was delivered */
} anland_present_ipc_status_t;

/* Prepare an idle reader. Every fd slot starts as -1 so a reset() on an unused
 * reader can never close stdin. */
void anland_present_ipc_reader_init(anland_present_ipc_reader_t *reader);

/* Discard any half-received message and close the FDs that came with it. Safe to
 * call repeatedly: it never closes the same fd twice. */
void anland_present_ipc_reader_reset(anland_present_ipc_reader_t *reader);

/* True when no partial message is buffered. */
bool anland_present_ipc_reader_idle(const anland_present_ipc_reader_t *reader);

/* Absolute CLOCK_MONOTONIC deadline `timeout_ms` from now. timeout_ms <= 0 means
 * "poll once without waiting". */
int64_t anland_present_ipc_deadline_after(int timeout_ms);

/* Advance the reader by at most one message.
 *
 * `payload` is the caller's persistent buffer for the message in progress; it
 * must stay valid (and unchanged) between calls for the same reader, because a
 * partially received payload is stored there.
 *
 * On ANLAND_PRESENT_IPC_MESSAGE the header is copied to out_header, the payload
 * length to out_payload_size, and any received FDs to out_fds (ownership moves
 * to the caller). The reader returns to the idle state.
 *
 * On ANLAND_PRESENT_IPC_INCOMPLETE the reader keeps its state so the next call
 * resumes exactly where this one stopped; any FDs already received stay open in
 * the reader.
 *
 * On ANLAND_PRESENT_IPC_ERROR the reader is reset (FDs closed) and the caller is
 * expected to tear the connection down. */
anland_present_ipc_status_t anland_present_ipc_reader_poll(
    int fd,
    anland_present_ipc_reader_t *reader,
    void *payload,
    size_t payload_capacity,
    int64_t deadline_ns,
    anland_present_msg_header_t *out_header,
    size_t *out_payload_size,
    int *out_fds,
    size_t fd_capacity,
    size_t *out_fd_count);

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_PRESENT_IPC_H */