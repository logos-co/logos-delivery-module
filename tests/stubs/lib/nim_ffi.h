/* nim-ffi poll model: what a host sees.
 *
 * A library built with -d:ffiPollMode exports, per library <lib>:
 *
 *   int <lib>_create[...](const uint8_t* req, size_t len, void** ctx_out, uint64_t* id_out);
 *   int <lib>_destroy(void* ctx);
 *   int <lib>_shutdown(void);
 *   int <lib>_<method>(void* ctx, const uint8_t* req, size_t len, uint64_t* id_out);
 *   int <lib>_poll(void* ctx, int32_t timeout_ms, const NimFfiMsg** msg);
 *   int <lib>_poll_fd(void* ctx);
 *   int <lib>_reverse_reply(void* ctx, uint64_t call_id, int ret, const uint8_t* payload, size_t len);
 *
 * A request is a CBOR map keyed by the proc's parameter names. NIMFFI_RET_OK
 * from a method promises exactly one REPLY message carrying *id_out; any other
 * return means no reply will come. Everything the library has to say --
 * replies, events, its own questions (REVERSE_CALL) -- comes out of
 * <lib>_poll, and <lib>_poll_fd is readable while a message waits. poll and
 * reverse_reply may be called from any host thread; a message is borrowed
 * until the next poll on the same context.
 *
 * Kept in step with ffi/ffi_msg.nim (cMsgDecl) and ffi/ret_codes.nim. */
#ifndef NIM_FFI_H
#define NIM_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NIMFFI_RET_OK
#define NIMFFI_RET_OK 0
#define NIMFFI_RET_ERR 1
#define NIMFFI_RET_MISSING_CALLBACK 2
#define NIMFFI_RET_STALE_WARN 3
#define NIMFFI_RET_TIMEOUT 4
#define NIMFFI_RET_CLOSED 5
#define NIMFFI_RET_INVALID_CTX 6
#define NIMFFI_RET_BUSY 7
#define NIMFFI_RET_QUEUE_FULL 8
#define NIMFFI_RET_TOO_LARGE 9
#endif

#ifndef NIMFFI_MSG_DECLARED
#define NIMFFI_MSG_DECLARED
typedef struct {
  uint32_t struct_size;   /* sizeof(NimFfiMsg) of the library; fields are only appended */
  uint32_t kind;          /* NIMFFI_MSG_* */
  uint64_t seq;           /* production order within the context */
  uint64_t id;            /* REPLY, STALE_WARN: the request id. REVERSE_CALL: the call id. Otherwise 0 */
  uint64_t name_id;       /* EVENT, REVERSE_CALL: which one. Otherwise 0 */
  uint64_t aux;
  int32_t  ret_code;      /* REPLY: NIMFFI_RET_OK, payload is CBOR; otherwise UTF-8 text */
  uint32_t flags;
  const uint8_t* payload; /* bare CBOR value; never NULL */
  size_t   len;
} NimFfiMsg;

#define NIMFFI_MSG_REPLY 1           /* id is the request; ret_code OK: payload is its CBOR, ERR: UTF-8 text */
#define NIMFFI_MSG_EVENT 2           /* name_id names it; payload is its CBOR */
#define NIMFFI_MSG_STALE_WARN 3      /* a request has run long; not terminal */
#define NIMFFI_MSG_REVERSE_CALL 4    /* the library asks; answer with <lib>_reverse_reply(id, ...) */
#define NIMFFI_MSG_NOT_RESPONDING 5  /* aux says why */
#define NIMFFI_MSG_RESPONDING 6
#define NIMFFI_MSG_CLOSED 7          /* the context is gone; every later poll fails */

#define NIMFFI_NOT_RESPONDING_HEARTBEAT 1
#define NIMFFI_NOT_RESPONDING_EVENT_QUEUE_FULL 2
#endif /* NIMFFI_MSG_DECLARED */

/* FNV-1a 64 of a wire name: what EVENT and REVERSE_CALL carry as name_id. */
static inline uint64_t nimffi_name_id(const char* wire)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  for (; *wire; ++wire) h = (h ^ (uint64_t)(unsigned char)*wire) * 0x100000001b3ULL;
  return h;
}

#ifdef __cplusplus
}
#endif
#endif /* NIM_FFI_H */
