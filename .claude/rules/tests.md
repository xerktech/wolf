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
- Never log a whole multi-megabyte payload (request or reply bodies, SSE events). CI pipes the
  console straight to the GitHub runner, and one line of a few MiB stalls the "Run tests" step
  until the 30 min job timeout. The suite itself finishes in ~20 s. Log at most the first 1 KiB.
