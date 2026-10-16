---
paths:
  - "src/moonlight-server/state/configTOML.cpp"
  - "src/moonlight-server/state/config.hpp"
  - "src/moonlight-server/api/endpoints.cpp"
---

# config.toml writes

- The API handles requests on a thread pool, so config.toml writes race (XERK-1962).
  - Every load-modify-save of the file goes through `update_config_file` under `config_file_mutex`.
  - Never call `rfl::toml::save` directly: it truncates in place, so a concurrent load reads a
    torn or empty file.
- `update_profiles` takes a function of the current profiles, run under that mutex.
  - Endpoints must build the new list from the argument, never from a `profiles->load()` taken
    earlier, or concurrent requests overwrite each other's changes.
- A failed save is logged, not thrown: an exception from an API handler terminates Wolf.

# Running wolf locally to drive the API

- Wolf segfaults at startup when no PulseAudio server is reachable; start one first
  (`pulseaudio -D -n --load="module-native-protocol-unix socket=<dir>/native auth-anonymous=1"
  --load=module-null-sink`) and set `PULSE_SERVER=unix:<dir>/native`.
- Without a GPU set `WOLF_RENDER_NODE=SOFTWARE` and install gstreamer1.0-plugins-{good,bad,ugly}
  (startup aborts if no H.264 encoder is found).
- Keep `WOLF_SOCKET_PATH` short (under ~100 chars): a long Unix socket path aborts startup with
  "File name too long".
