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
- Don't "fix" this by having `fire_event` copy the handlers and call them outside the lock.
  - Handlers capture stack references (e.g. `runners/process.cpp`'s `&group_proc`); the lock is what
    keeps `unregister()` from returning while a call is still running.
- Tests: `tests/testGSTPlugin.cpp` "Stopping a stuck pipeline doesn't block the event bus" (real
  `start_streaming_video`/`audio` handlers, `identity sleep-time` as the stuck thread).
  - Fire only after the first buffer has passed: an EOS during preroll can deadlock the pipeline on
    its STATE_LOCK, which hangs the test rather than failing it.
