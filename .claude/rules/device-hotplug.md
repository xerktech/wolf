---
paths:
  - src/moonlight-server/sessions/lobbies.cpp
  - src/moonlight-server/control/input_handler.cpp
  - src/moonlight-server/runners/docker.cpp
  - src/core/src/platforms/all/helpers/helpers/tsqueue.hpp
  - tests/testWolfAPI.cpp
---

# Device plug/unplug ordering

- A plug reaches a Docker runner through its `plugged_devices_queue`, drained every ~500ms; an unplug arrives
  as an `UnplugDeviceEvent` handled at once. Unplugs must never run before their plug's commands, or the
  container keeps a device nobody removes.
  - `runners/docker.cpp` holds one mutex over the queue drain + plug commands and over the unplug handler;
    an unplug whose plug is still queued drops it from the queue instead of running remove commands.
  - Match plug and unplug with `events::is_same_device`, never `==` on the udev events: `USEC_INITIALIZED`
    is a seconds timestamp taken each time a pad's events are built (XERK-2409 QA: 14/200 ghosts).
- Lobby joins: the lobby relay only forwards a session's unplug once the session is in `connected_sessions`,
  but the join queues the pads' plugs before that (so a relayed unplug can't overtake them).
  - The join re-reads `session->joypads` after connecting and unplugs from the lobby any snapshot pad (queued
    or taken over) that has gone. control's unplugs (`take_joypad`) erase the pad before firing, so a pad
    still listed at the re-check has its event still to come, and the relay forwards it.
  - Erase-then-fire has a cost on leave: same-type handlers run newest-first, so the session runner's
    exec runs between the erase and the relay's check; a leave in that window neither snapshots the pad
    nor gets it relayed → ghost in the lobby (XERK-2409 QA: 53/200). XERK-2410 tracks the leave side.
  - A pad unplugged right at the connect can get both the relayed unplug and the join's; the second remove of a
    gone node fails its exec and logs a warning.
- Tests: `tests/testWolfAPI.cpp` "A joypad unplugged while its session joins a lobby is unplugged from the
  lobby" (needs `/dev/uinput`; skips on the TSan leg),
  "An unplug matches its device's plug whenever its udev events were built".
