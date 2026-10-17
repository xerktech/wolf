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
