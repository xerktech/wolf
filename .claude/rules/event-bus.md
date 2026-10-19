---
paths:
  - src/moonlight-server/**
  - src/core/src/core/gstreamer.hpp
  - tests/testGSTPlugin.cpp
---

# Event bus handlers

- `dp::event_bus::fire_event` runs every handler while holding the bus lock (shared); register and
  unregister take it exclusively.
  - A handler that blocks stalls every runner start/stop server-wide (XERK-1966: a stuck pipeline's
    synchronous EOS from HTTPS `/cancel` hung all launches and RTSP).
  - Never block in a handler: hand slow or lock-taking work to another thread.
- Send EOS to a pipeline with `gstreamer::send_eos_async`, never `gst_element_send_event` from a
  handler. A synchronous EOS waits on the source's stream lock, which a stuck streaming thread holds.
  - `send_eos_async` sends the EOS to each source, never to the pipeline: a bin holds its STATE_LOCK
    while the sources take it, and a still-prerolling pipeline needs that lock to finish → it hangs
    forever. Tests: `tests/testGSTPlugin.cpp` "Stopping a pipeline during preroll ends it".
  - Other events into a pipeline (e.g. the IDR's force-key-unit) go through `send_message_async`.
- `SwitchStreamProducerEvents` stays synchronous (`g_object_set` "listen-to"): an async switch runs on
  an unordered thread pool, so a quick join + leave could land in the wrong order.
- Don't "fix" this by having `fire_event` copy the handlers and call them outside the lock.
  - Handlers capture stack references (e.g. `runners/process.cpp`'s `&group_proc`); the lock is what
    keeps `unregister()` from returning while a call is still running.
- Tests: `tests/testGSTPlugin.cpp` "Stopping a stuck pipeline doesn't block the event bus" (real
  `start_streaming_video`/`audio` handlers, `identity sleep-time` as the stuck thread).
  - A test whose pipeline may hang runs it on a detached thread + promise, not `std::async`: the
    async future's destructor would block the whole suite instead of failing the test.
