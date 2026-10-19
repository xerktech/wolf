---
paths:
  - "src/moonlight-server/state/configTOML.cpp"
  - "src/moonlight-server/state/config.hpp"
  - "src/moonlight-server/api/endpoints.cpp"
  - "src/moonlight-server/state/default/*.toml"
---

# config.toml writes

- The API handles requests on a thread pool, so config.toml writes race (XERK-1962).
  - Every load-modify-save of the file goes through `update_config_file` under `config_file_mutex`.
  - Never call `rfl::toml::save` directly: it truncates in place, so a concurrent load reads a
    torn or empty file.
- `update_profiles` / `update_client_settings` take a function of the current value, run under
  that mutex.
  - Endpoints must build the new value from the argument, never from a snapshot taken earlier,
    or concurrent requests overwrite each other's changes.
- The atomic replace copies the file's mode and owner onto the temp file and falls back to an
  in-place rewrite when it can't (unwritable dir, bind-mounted file, foreign owner): don't drop
  either, config.toml holds paired-client certs.
  - Never fall back in place when writing the content failed (disk full): truncating the
    original would leave Wolf unable to boot.
  - The temp file is unlinked and opened `O_EXCL | O_NOFOLLOW`: a planted symlink or FIFO at
    `<config>.tmp` must not be followed.
- A failed save is logged, not thrown: an exception from an API handler terminates Wolf.
  - It is returned instead (`pair`/`unpair`/`update_profiles` → false, `update_client_settings` →
    `ConfigUpdate::NOT_SAVED`): the API answers 500, since the change is lost on the next restart.
- A config.toml that loads without a uuid while Wolf has one was emptied or replaced under us: never save
  over it (DefaultIfMissing would write an empty uuid/hostname and drop the other paired clients).
- Tests: `tests/testMoonlight.cpp` "LocalState reports a config file it couldn't save"; `tests/testWolfAPI.cpp`
  "API reports a config change that couldn't be saved".
- Tests: `tests/testMoonlight.cpp` "LocalState concurrent updates" (lost updates, mode, planted
  temp symlink/FIFO).

# Default config (config.v7.toml)

- `config.include.toml` is generated from `config.v7.toml` by `make_includable` at cmake
  configure time only, and it is checked in: edit both, and re-run cmake (not just ninja).
- Every encoder needs `video_params` and `video_params_zero_copy`, either inline or via
  `[gstreamer.video.defaults.<plugin_name>]`; a missing one resolves to "" and the pipeline fails
  to parse (XERK-2026).
  - Tests: `tests/testMoonlight.cpp` "Default config: every encoder has video_params".
- Only `config_version <= 6` is migrated on load: a fix to the defaults never reaches an existing
  v7 config.toml without a version bump.
  - Exception: an encoder whose params resolve to "" falls back at load to the same `plugin_name`
    in the embedded default (`encoder_video_params`, XERK-2043), so pre-XERK-2026 v7 configs
    stream. Keep the default's encoders complete; that's what the fallback serves.
  - Tests: `tests/testMoonlight.cpp` "Encoder video_params fall back to the embedded default".

# Running wolf locally to drive the API

- Without a reachable PulseAudio server Wolf starts with no audio (XERK-2005); for audio start one first
  (`pulseaudio -D -n --load="module-native-protocol-unix socket=<dir>/native auth-anonymous=1"
  --load=module-null-sink`) and set `PULSE_SERVER=unix:<dir>/native`.
- Without a GPU set `WOLF_RENDER_NODE=software` (lowercase: `SOFTWARE` does not work) and install
  gstreamer1.0-plugins-{good,bad,ugly}
  (startup aborts if no H.264 encoder is found).
- Keep `WOLF_SOCKET_PATH` short (under ~100 chars): with a long Unix socket path Wolf starts
  without the API ("Unable to start the Wolf API server … File name too long").
