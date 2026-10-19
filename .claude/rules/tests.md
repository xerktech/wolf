---
paths:
  - "tests/**"
---

# wolftests pitfalls

- `config.test.EDITED.toml` in the build's `tests/` dir is copied from `tests/assets/config.test.toml`
  only at CMake configure time, and the API tests mutate it in order.
  - Running one API test on its own (e.g. `./wolftests "APPs APIs"`) reuses the previous run's file and
    can fail spuriously; re-copy the asset first.
  - The same goes for `config.test.toml` after editing the asset: re-copy or re-configure.
- `Lobbies APIs` / lobby tests time out ("Lobby setup timed out") without the `waylanddisplaysrc`
  GStreamer plugin (gst-wayland-display) on `GST_PLUGIN_PATH`; that is environmental, not a regression.
- `[GSTREAMER]` tests that parse `interpipesrc` (e.g. "streaming: a stream listens to its own
  instance's producers") fail with `"" == ...` when gst-interpipe isn't installed (CI builds it);
  environmental, not a regression.
- Build with `ninja -j2` on shared hosts: a full-parallel build of wolftests uses >12 GiB.
- Never log a whole multi-megabyte payload (request or reply bodies, SSE events). CI pipes the
  console straight to the GitHub runner, and one line of a few MiB stalls the "Run tests" step
  until the 30 min job timeout. The suite itself finishes in ~20 s. Log at most the first 1 KiB.
- Wayland tests (`tests/platforms/linux/wayland-display.cpp`): `TestWaylandDisplay` waits for the
  pipeline's first buffer before returning. gst-wayland-display creates its output only at caps
  negotiation and drops a toplevel committed before that, so the window never maps and gets no
  input (failed ~1/2 runs under load). Don't remove that wait.
  - The client is single-threaded and its queues only fill while it dispatches: pop with
    `pop_event`, not a bare `queue->pop()` after one roundtrip.
- API tests bind `wolf.sock` in `api_runtime_dir()`, one short mkdtemp dir per run, never
  `XDG_RUNTIME_DIR`: a long
  path overflows sun_path and aborts the binary, a shared one lets concurrent runs on a host reach each other's
  server. Start servers with `start_api_server` (waits until it accepts), never a fixed sleep.
- Don't `setenv()` in a test: earlier tests' detached server threads may be reading the environment.
