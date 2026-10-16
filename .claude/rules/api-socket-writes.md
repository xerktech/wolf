---
paths:
  - src/moonlight-server/api/**
  - tests/testWolfAPI.cpp
---

# Unix-socket API: writing to sockets

- Every write goes through `send_data` / `sse_broadcast`. Never call `async_write` on an API
  socket directly.
  - `async_write` returns before the bytes are sent, so the buffer must be owned by the queue, never
    a view of a local or a temporary. The bug only shows on replies larger than one socket write
    (~64 KiB); small replies hide it.
  - Two `async_write`s in flight on one socket interleave their bytes. The per-socket
    `write_queue` keeps exactly one in flight.
- `state_->sockets` and every `write_queue` belong to the io_context thread. Endpoints, event-bus
  publishers and the docker-pull thread run elsewhere, so they `dispatch` onto it first.
- Tests: `tests/testWolfAPI.cpp` "APPs APIs" sends a 4 MiB reply under `M_PERTURB`; the SSE
  cases cover interleaving and the stalled-reader cap. The concurrency (dispatch) is caught only by
  a live stress rig or TSan, not by the unit tests.
- Building locally: `ninja -j3` at most. Each reflect-cpp TU needs ~2 GiB, and the host's memory
  guard kills large parallel builds.
- ASan: set `ASAN_OPTIONS=allow_user_segv_handler=0`. `wolf.cpp` installs its own SIGSEGV handler,
  which otherwise swallows ASan's report.
