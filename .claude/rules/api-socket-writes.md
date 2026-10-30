---
paths:
  - src/moonlight-server/api/**
  - tests/testWolfAPI.cpp
---

# Unix-socket API: writing to sockets

- Every write goes through `send_data` / `sse_broadcast`, or through `queue_write` when already on the
  io_context thread. Never call `async_write` on an API socket directly.
- `sse_keepalive` queues only to sockets with an empty `write_queue`. Don't route it through
  `sse_broadcast`: a keepalive queued behind a backlog over `MAX_WRITE_QUEUE_BYTES` drops a
  subscriber that is still reading. Tests: "SSE APIs large events".
  - `async_write` returns before the bytes are sent, so the buffer must be owned by the queue, never
    a view of a local or a temporary. The bug only shows on replies larger than one socket write
    (~64 KiB); small replies hide it.
  - Two `async_write`s in flight on one socket interleave their bytes. The per-socket
    `write_queue` keeps exactly one in flight.
- The keepalive interval is a `start_server` parameter (default `SSE_KEEPALIVE_INTERVAL`, 15 s) so
  tests can shorten it; a test waiting on 15 s would slow the suite. Tests: "SSE APIs idle subscriber
  gets keepalives" (the only test that fails if keepalives stop being sent).
- `state_->sockets` and every `write_queue` belong to the io_context thread. Endpoints, event-bus
  publishers and the docker-pull thread run elsewhere, so they `dispatch` onto it first.
- `queue_write` posts itself onto the io_context thread when called from another one: a direct call
  from an endpoint thread is safe, not a race. Iterating `state_->sockets` has no such guard.
- Tests: `tests/testWolfAPI.cpp` "APPs APIs" sends a 4 MiB reply under `M_PERTURB`; the SSE
  cases cover interleaving and the stalled-reader cap. A race on `state_->sockets` is caught only
  by the CI TSan leg (the `[API]` tests under TSan), not by the unit tests' assertions.
- Building locally: `ninja -j3` at most. Each reflect-cpp TU needs ~2 GiB, and the host's memory
  guard kills large parallel builds.
- ASan: set `ASAN_OPTIONS=allow_user_segv_handler=0`. `wolf.cpp` installs its own SIGSEGV handler,
  which otherwise swallows ASan's report.
- The io_context thread can outlive `main` (tests detach it), so it must never touch a static with a
  destructor: TSan reported `sse_keepalive` copying its static keepalive buffer after exit freed it.
- Never `close()` a socket right after queueing its reply: it cancels the write in flight. A reply's socket
  closes by itself once the write completes and the last reference goes.

# API contracts

- `docs/modules/dev/partials/spec.json` must match the served `/api/v1/openapi-schema`; when you add an
  endpoint, a field or a response code, regenerate it. Tests: "The OpenAPI spec in the docs matches the served one"
  (writes the served schema to `openapi-spec.served.json` in the test dir on failure).
- `sessions/add` without `client_id` gives each session a unique dummy cert, hence its own session_id; a second
  session for a client that already has one is refused with 409. Every session event is keyed by session_id, so
  two sessions under one id can't be told apart (fenrir omits `client_id`).
  - Decide the 409 inside the `running_sessions->update` lambda (compare-and-insert), never from an earlier
    `load()`: concurrent adds for one client all pass a separate check. `atom::update` may rerun the lambda,
    so set the flag on every run. Tests: "Malformed IDs are rejected without crashing" (16 parallel adds).
- `sessions/add` requires Moonlight's key material: `aes_key` 32 hex digits, `aes_iv` a decimal; `sessions/start`
  checks them in an encrypted `audio_session`. The audio payloader can't run without a 16-byte key (an empty
  `aes_key=` reaches it as NULL); it now errors out the stream, the API must not let one reach it.
- `apps/add` with an id already in use is 409, checked inside the profiles update for the same reason.
- A request (headers + body) is capped at `MAX_REQUEST_BYTES`; a larger Content-Length gets 413 before any
  body is read. Never make a read buffer unbounded again: it read or waited for any length, growing
  quadratically. Tests: "API refuses requests it can't read with a JSON error".
  - Headers alone are capped at `MAX_HEADER_BYTES` (431): every connection buffers them, so they must stay small.
  - Bodies being read share `MAX_PENDING_BODY_BYTES` over all connections (503): a per-connection cap alone let
    N connections hold N x 64 MiB. The body is read into a reserved string, so a bare Content-Length costs no RSS.
  - Bytes already in the socket are read at once; a body that arrived whole never touches the budget, so idle
    holders can't refuse normal requests.
  - A body that needs an async read is checked and charged in full, never only what's left to read: charging the
    rest let N connections each park a socket buffer outside the budget, and checking the rest let the counter
    pass the limit and wrap. Tests: the 503 block of "API refuses requests it can't read with a JSON error".
- The 3-argument `send_http` labels every non-empty body `application/json`: send other content types
  (icons, SSE) through the overload with explicit headers. The status line's reason follows the code.
- A malformed id or a body that doesn't parse is 400, a well-formed but unknown id 404, a wrong lobby PIN 403,
  a full lobby 409; 500 is for Wolf's own failures (a save that failed, a timeout).
- Lobby joins and leaves are serialised by `membership_lock` in `setup_lobbies_handlers`: a join reads the
  session's current lobby, leaves it and joins the new one. Tests: "Joining a second lobby leaves the first one".
  - A lobby stop holds it too, from its snapshot of `connected_sessions` to the lobby's removal: a join in
    between left the session in a lobby that's gone (XERK-2440). Tests: "Joining a lobby while it stops
    doesn't leave the session in the stopped lobby".
