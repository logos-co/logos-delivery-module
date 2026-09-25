// nim-ffi poll model: the host side, once.
//
// A poll-mode library hands the host every message -- method replies, events,
// and the library's own questions (REVERSE_CALL) -- through one queue per
// context, and a file descriptor that is readable while the queue is not
// empty. nim_ffi::Host owns that queue for one context:
//
//   - call() submits a request and then pumps the queue on the calling thread
//     until that call's reply arrives. Everything else that comes out
//     meanwhile (events, other replies, reverse calls) is dispatched as it is
//     seen, so a library that must ask the host something before it can
//     answer is served from inside the wait. Nothing can deadlock on a
//     message this thread must itself deliver.
//   - Between calls the host's own event loop should watch fd() and call
//     drain() when it is readable (a QSocketNotifier, a poll(2) set, ...).
//
// Host owns no thread. reverseReply() may be called from any thread (the
// library guarantees that side); everything else runs on the thread that
// pumps, which is the thread that owns the host's event loop. Payloads are
// bytes: the CBOR (or, for a text reply, the UTF-8) is the caller's to decode
// with whatever codec it already has.
//
// Header-only; requires nim_ffi.h beside it.
#ifndef NIM_FFI_HOST_HPP
#define NIM_FFI_HOST_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "nim_ffi.h"

namespace nim_ffi {

// The five fixed exports of one library, bound by the host.
struct Library {
  int (*create)(const uint8_t* req, size_t len, void** ctx_out, uint64_t* id_out);
  int (*destroy)(void* ctx);
  int (*poll)(void* ctx, int32_t timeout_ms, const NimFfiMsg** msg);
  int (*poll_fd)(void* ctx);
  int (*reverse_reply)(void* ctx, uint64_t call_id, int ret, const uint8_t* payload, size_t len);
};

using Method = int (*)(void* ctx, const uint8_t* req, size_t len, uint64_t* id_out);
using Bytes = std::vector<uint8_t>;

// A request's outcome. `ret` is the library's code; on NIMFFI_RET_OK `payload`
// is the reply's CBOR, otherwise `error` is the library's text (or the host's,
// for a submit that was not accepted or a wait that ran out).
struct Reply {
  int ret = NIMFFI_RET_ERR;
  Bytes payload;
  std::string error;
};

class Host {
 public:
  // An event: its wire name id and its payload, borrowed for the call.
  using EventHandler = std::function<void(uint64_t name_id, const uint8_t* payload, size_t len)>;
  // A question from the library: answer it, from any thread, with reverseReply.
  using ReverseHandler =
      std::function<void(uint64_t call_id, uint64_t name_id, const uint8_t* args, size_t len)>;
  // A reply nobody waits for, handed on arrival (see submit).
  using Done = std::function<void(const Reply&)>;

  explicit Host(Library lib) : lib_(lib) {}
  ~Host() { destroy(); }
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  void onEvent(EventHandler h) { on_event_ = std::move(h); }
  void onReverseCall(ReverseHandler h) { on_reverse_ = std::move(h); }

  // Creates the context and pumps until the constructor's reply. On a
  // non-OK reply the context is destroyed again and alive() stays false.
  Reply create(const Bytes& req, std::chrono::milliseconds timeout) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    Reply r;
    if (ctx_) {
      r.error = "context already created";
      return r;
    }
    void* ctx = nullptr;
    uint64_t id = 0;
    const int rc = lib_.create(req.data(), req.size(), &ctx, &id);
    if (rc != NIMFFI_RET_OK || !ctx) {
      r.ret = rc;
      r.error = "create: not accepted, rc=" + std::to_string(rc);
      return r;
    }
    ctx_ = ctx;
    r = waitFor(id, timeout);
    if (r.ret != NIMFFI_RET_OK) {
      if (ctx_) lib_.destroy(ctx_);
      ctx_ = nullptr;
      settled_.clear();
      done_.clear();
    }
    return r;
  }

  void destroy() {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (ctx_) {
      lib_.destroy(ctx_);
      ctx_ = nullptr;
    }
    settled_.clear();
    done_.clear();
  }

  bool alive() const { return ctx_ != nullptr; }
  void* ctx() const { return ctx_; }
  // Readable while a message waits; -1 without a context.
  int fd() const { return ctx_ ? lib_.poll_fd(ctx_) : -1; }

  // Submits `method` and pumps this thread until its reply, or `timeout`.
  Reply call(Method method, const Bytes& req, std::chrono::milliseconds timeout) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    Reply r;
    if (!ctx_) {
      r.ret = NIMFFI_RET_INVALID_CTX;
      r.error = "no context";
      return r;
    }
    uint64_t id = 0;
    const int rc = method(ctx_, req.data(), req.size(), &id);
    if (rc != NIMFFI_RET_OK) {
      // no reply will come for this call; the code says why
      r.ret = rc;
      r.error = "not accepted, rc=" + std::to_string(rc);
      return r;
    }
    r = waitFor(id, timeout);
    if (r.ret == NIMFFI_RET_TIMEOUT && r.error.empty()) {
      r.error = "no reply within " + std::to_string(timeout.count()) + " ms";
    }
    return r;
  }

  // Submits without waiting and hands the reply to `done` when the pump sees
  // it. Returns the library's code; `done` never runs unless it is OK.
  int submit(Method method, const Bytes& req, Done done) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (!ctx_) return NIMFFI_RET_INVALID_CTX;
    uint64_t id = 0;
    const int rc = method(ctx_, req.data(), req.size(), &id);
    if (rc == NIMFFI_RET_OK && done) done_[id] = std::move(done);
    drain();  // whatever is already queued, including a reply that came at once
    return rc;
  }

  // Answers a REVERSE_CALL. Any thread. `payload` is the reply's CBOR on
  // NIMFFI_RET_OK, otherwise UTF-8 text.
  void reverseReply(uint64_t call_id, int ret, const uint8_t* payload, size_t len) {
    void* ctx = ctx_;  // read without the lock: replies come from other threads
    if (!ctx) return;
    lib_.reverse_reply(ctx, call_id, ret, payload, len);
  }
  void reverseReply(uint64_t call_id, int ret, const Bytes& payload) {
    reverseReply(call_id, ret, payload.data(), payload.size());
  }
  void reverseReply(uint64_t call_id, int ret, const std::string& text) {
    reverseReply(call_id, ret, reinterpret_cast<const uint8_t*>(text.data()), text.size());
  }

  // Dispatches every message queued right now. What the event loop calls
  // when fd() is readable.
  void drain() {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (!ctx_) return;
    const NimFfiMsg* m = nullptr;
    while (lib_.poll(ctx_, 0, &m) == NIMFFI_RET_OK && m) {
      dispatch(m);
      m = nullptr;
    }
  }

 private:
  // Time slice of one blocking poll while waiting: long enough to stay idle
  // cheaply, short enough that the deadline is honoured.
  static constexpr int32_t kSliceMs = 50;

  static Reply decodeReply(const NimFfiMsg* m) {
    Reply r;
    r.ret = m->ret_code;
    if (m->ret_code == NIMFFI_RET_OK) {
      r.payload.assign(m->payload, m->payload + m->len);
    } else {
      r.error.assign(reinterpret_cast<const char*>(m->payload), m->len);
    }
    return r;
  }

  Reply waitFor(uint64_t id, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      if (auto it = settled_.find(id); it != settled_.end()) {
        Reply r = std::move(it->second);
        settled_.erase(it);
        return r;
      }
      Reply r;
      if (!ctx_) {
        r.ret = NIMFFI_RET_CLOSED;
        r.error = "context closed while waiting";
        return r;
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) {
        r.ret = NIMFFI_RET_TIMEOUT;
        return r;
      }
      const NimFfiMsg* m = nullptr;
      const int rc = lib_.poll(ctx_, static_cast<int32_t>(std::min<int64_t>(left.count(), kSliceMs)), &m);
      if (rc == NIMFFI_RET_OK && m) {
        dispatch(m);
      } else if (rc != NIMFFI_RET_TIMEOUT && rc != NIMFFI_RET_OK) {
        r.ret = rc;
        r.error = "poll rc=" + std::to_string(rc);
        return r;
      }
    }
  }

  void dispatch(const NimFfiMsg* m) {
    switch (m->kind) {
      case NIMFFI_MSG_REPLY:
        if (auto it = done_.find(m->id); it != done_.end()) {
          Done done = std::move(it->second);
          done_.erase(it);
          done(decodeReply(m));
        } else {
          settled_[m->id] = decodeReply(m);  // kept until its caller asks
        }
        break;
      case NIMFFI_MSG_EVENT:
        if (on_event_) on_event_(m->name_id, m->payload, m->len);
        break;
      case NIMFFI_MSG_REVERSE_CALL:
        if (on_reverse_) {
          on_reverse_(m->id, m->name_id, m->payload, m->len);
        } else {
          reverseReply(m->id, NIMFFI_RET_ERR, std::string("no handler for reverse calls"));
        }
        break;
      case NIMFFI_MSG_CLOSED:
        ctx_ = nullptr;  // nothing more will come; waiters see NIMFFI_RET_CLOSED
        break;
      default:
        break;  // STALE_WARN and the liveness ticks: the deadline decides
    }
  }

  Library lib_;
  void* ctx_ = nullptr;
  std::recursive_mutex lock_;  // the queue is pumped from one thread at a time
  std::map<uint64_t, Reply> settled_;  // replies seen before their waiter asked
  std::map<uint64_t, Done> done_;      // replies nobody waits for
  EventHandler on_event_;
  ReverseHandler on_reverse_;
};

}  // namespace nim_ffi

#endif  // NIM_FFI_HOST_HPP
