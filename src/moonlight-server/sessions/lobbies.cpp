#include <immer/vector_transient.hpp>
#include <mutex>
#include <sessions/common.hpp>
#include <sessions/handlers.hpp>
#include <state/config.hpp>
#include <state/data-structures.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions {

/**
 * Unplugs from `lobby` the joypads it keeps for `session_id` (Lobby::parked_joypads) that aren't in `keep`, and forgets
 * them: the lobby's app hears of each, before its device goes away with the last reference.
 */
void release_parked_joypads(const std::shared_ptr<events::EventBusType> &ev_bus,
                            const events::Lobby &lobby,
                            std::size_t session_id,
                            const events::JoypadList &keep = {}) {
  std::shared_ptr<const events::ParkedJoypads> parked;
  lobby.parked_joypads->update([&](const auto &all) {
    if (auto found = all.find(session_id)) {
      parked = *found;
      return all.erase(session_id);
    }
    return all;
  });
  if (!parked) {
    return;
  }
  for (auto [joypad_nr, joypad] : parked->joypads) {
    if (auto kept = keep.find(joypad_nr); kept && *kept == joypad) {
      continue;
    }
    events::UnplugDeviceEvent unplug_ev{.session_id = lobby.id};
    std::visit(
        [&unplug_ev](auto &pad) {
          unplug_ev.udev_events = pad.get_udev_events();
          unplug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
        },
        *joypad);
    ev_bus->fire_event(immer::box<events::UnplugDeviceEvent>{unplug_ev});
  }
}

/**
 * @brief Removes the StreamSession from the input Lobby and switches everything to the original session
 *
 * @note Leaving a lobby may have side effects,
 * like terminating the lobby if it becomes empty or triggering additional events.
 */
void leave_lobby(const std::shared_ptr<events::EventBusType> &ev_bus,
                 const events::Lobby &lobby,
                 const events::StreamSession &session) {
  logs::log(logs::info, "[LOBBY] Session {} leaving lobby {}", session.session_id, lobby.id);
  // Remove the current session from the lobby list
  lobby.connected_sessions->update([session](const immer::vector<immer::box<std::string>> &connected_sessions) {
    return connected_sessions | //
           ranges::views::filter([session](const immer::box<std::string> &session_id) {
             return *session_id != std::to_string(session.session_id);
           }) | //
           ranges::to<immer::vector<immer::box<std::string>>>();
  });

  // Switch over mouse and keyboard to use the original session wayland server
  auto wl_state = session.wayland_display->load();
  session.mouse->emplace(virtual_display::WaylandMouse(wl_state));
  session.keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
  session.touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));

  // Switch over all joypads present in the lobby back into the original session, unless the lobby keeps them for the
  // session's next stream: then the lobby's app must not see them unplugged
  auto parked = lobby.parked_joypads->load();
  events::JoypadList joypads = parked->find(session.session_id) ? events::JoypadList{} : *session.joypads->load();
  for (auto [_joypad_nr, joypad] : joypads) {
    // Plug them into original session
    events::PlugDeviceEvent plug_ev{.session_id = std::to_string(session.session_id)};
    std::visit(
        [&plug_ev](auto &pad) {
          plug_ev.udev_events = pad.get_udev_events();
          plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
        },
        *joypad);
    ev_bus->fire_event(immer::box<events::PlugDeviceEvent>(plug_ev));
    // Unplug them from the current lobby
    ev_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
        events::UnplugDeviceEvent{.session_id = lobby.id,
                                  .udev_events = plug_ev.udev_events,
                                  .udev_hw_db_entries = plug_ev.udev_hw_db_entries}});
  }
  // TODO: hotplug pen_tablet and touch_screen

  // Switch audio/video gstreamer stream producers
  ev_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>{
      events::SwitchStreamProducerEvents{.session_id = session.session_id,
                                         .interpipe_src_id = state::producer_name(session)}});

  if (lobby.stop_when_everyone_leaves && lobby.connected_sessions->load()->size() == 0) {
    // Nobody left in the lobby, and it's set to stop when everyone leaves
    ev_bus->fire_event(immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby.id}});
  }
}

immer::vector<immer::box<events::EventBusHandlers>>
setup_lobbies_handlers(const immer::box<state::AppState> &app_state,
                       const std::string &runtime_dir,
                       const std::optional<AudioServer> &audio_server) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;
  // Serialises joins and leaves: a join reads which lobby the session is in, leaves it and joins the new one, so
  // concurrent joins would otherwise leave a session in two lobbies or listed twice in one.
  // Recursive: leaving can stop a lobby, whose handler fires the LeaveLobbyEvent of its other sessions on this thread.
  auto membership_lock = std::make_shared<std::recursive_mutex>();

  // On create lobby event
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::CreateLobbyEvent>>(
      [=](const immer::box<events::CreateLobbyEvent> &lobby_settings) {
        logs::log(logs::info, "[LOBBY] Creating new lobby");
        auto ev_bus = app_state->event_bus;

        auto lobby = std::make_shared<events::Lobby>(
            events::Lobby{.id = lobby_settings->id,
                          .name = lobby_settings->name,
                          .started_by_profile_id = lobby_settings->profile_id,
                          .icon_png_path = lobby_settings->icon_png_path,
                          .multi_user = lobby_settings->multi_user,
                          .pin = lobby_settings->pin,
                          .stop_when_everyone_leaves = lobby_settings->stop_when_everyone_leaves,
                          .runner = lobby_settings->runner});
        app_state->lobbies->update(
            [lobby](const immer::vector<events::Lobby> &lobbies) { return lobbies.push_back(*lobby); });

        { // Start Wayland compositor and Gstreamer producer pipeline
          logs::log(logs::debug, "[LOBBY] Create wayland compositor");

          std::shared_ptr<boost::promise<streaming::WaylandDisplayReady>> on_ready =
              std::make_shared<boost::promise<streaming::WaylandDisplayReady>>();

          std::thread([lobby, lobby_settings, ev_bus, on_ready, gst_context = app_state->gst_context]() {
            streaming::start_video_producer(lobby->id,
                                            lobby->id,
                                            lobby_settings->video_settings.video_producer_buffer_caps,
                                            lobby_settings->video_settings.wayland_render_node,
                                            {.width = lobby_settings->video_settings.width,
                                             .height = lobby_settings->video_settings.height,
                                             .refreshRate = lobby_settings->video_settings.refresh_rate},
                                            gst_context,
                                            on_ready,
                                            ev_bus);
          }).detach();

          // For the runner: true once the lobby was stopped (see events::Runner::run)
          std::function<bool()> stopped = [lobbies = app_state->lobbies, id = lobby->id]() {
            return !state::get_lobby_by_id(lobbies->load(), id);
          };
          auto w_display_ready = on_ready->get_future().then(
              [lobby, runtime_dir, ev_bus, audio_server, lobby_settings, host = app_state->host, stopped](auto fut) {
                streaming::WaylandDisplayReady ready = fut.get();

                auto wl_state =
                    virtual_display::create_wayland_display(ready.wayland_plugin, ready.wayland_socket_name);
                // Set the wayland display
                lobby->wayland_display->store(wl_state);

                if (!wait_for_wayland_socket(runtime_dir, ready.wayland_socket_name, true)) {
                  logs::log(logs::error,
                            "[LOBBY] Wayland socket {} was not ready, aborting runner startup",
                            ready.wayland_socket_name);
                  ev_bus->fire_event(immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby->id}});
                  return;
                }

                if (lobby->runner_claimed->exchange(true)) {
                  // Stopped while setting up (e.g. the API create call timed out): don't start the runner.
                  // Stop again, for the producer pipeline that may have come up after the first stop.
                  logs::log(logs::warning, "[LOBBY] Lobby {} was stopped during setup", lobby->id);
                  lobby->wayland_display->store(nullptr);
                  ev_bus->fire_event(immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby->id}});
                  return;
                }

                { // Start runner
                  logs::log(logs::debug, "[LOBBY] Start runner");
                  auto full_path = std::filesystem::path(host->local_base_state_folder) /
                                   lobby_settings->runner_state_folder;
                  logs::log(logs::debug, "Host app state folder: {}, creating paths", full_path.string());
                  std::filesystem::create_directories(full_path);

                  std::thread([=]() {
                    start_runner(lobby->runner,
                                 lobby->plugged_devices_queue,
                                 immer::box<RunnerArgs>{RunnerArgs{
                                     .session_id = lobby->id,
                                     .video_settings = lobby_settings->video_settings,
                                     .wayland_display = lobby->wayland_display->load(),
                                     .audio_server = audio_server,
                                     .audio_sink = lobby->audio_sink->load(),
                                     .host = host,
                                     .app_local_state_folder = full_path.string(),
                                     .app_host_state_folder = std::filesystem::path(host->host_base_state_folder) /
                                                              lobby_settings->runner_state_folder,
                                     .xdg_runtime_dir = runtime_dir,
                                     .client_settings = lobby_settings->client_settings,
                                     .is_stopped = stopped}});
                    // Runner process ended, stop the lobby
                    lobby->wayland_display->store(nullptr);

                    ev_bus->fire_event<immer::box<events::StopLobbyEvent>>(
                        immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby->id}});
                  }).detach();
                }

                lobby_settings->on_setup_over.get()->set_value(true);
              });
        }

        { // Create audio virtual sink
          logs::log(logs::debug, "[LOBBY] Create audio virtual sink");
          auto pulse_sink_name = fmt::format("{}{}", VIRTUAL_SINK_PREFIX, lobby->id);
          if (audio_server && audio_server->server) {
            auto channel_count = lobby_settings->audio_settings.channel_count;
            auto v_device = audio::create_virtual_sink(
                audio_server->server,
                audio::AudioDevice{.sink_name = pulse_sink_name, .mode = state::get_audio_mode(channel_count, true)});

            lobby->audio_sink->store(v_device);

            // Start Gstreamer producer pipeline
            std::thread([lobby, audio_server = audio_server->server, ev_bus, channel_count]() {
              auto sink_name = fmt::format("{}{}.monitor", VIRTUAL_SINK_PREFIX, lobby->id);
              streaming::start_audio_producer(lobby->id,
                                              lobby->id,
                                              ev_bus,
                                              channel_count,
                                              sink_name,
                                              audio::get_server_name(audio_server));
            }).detach();
          }
        }
      }));

  // When a Moonlight client joins a lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::JoinLobbyEvent>>(
      [=](const immer::box<events::JoinLobbyEvent> &join_lobby_event) {
        std::lock_guard membership_guard(*membership_lock);
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), join_lobby_event->lobby_id);
        auto sessions = app_state->running_sessions->load();
        auto session = state::get_session_by_id(sessions.get(), join_lobby_event->moonlight_session_id);

        if (!lobby || !session) {
          logs::log(logs::error,
                    "[LOBBY] Failed to join lobby: lobby {} or session {} not found",
                    join_lobby_event->lobby_id,
                    join_lobby_event->moonlight_session_id);
          join_lobby_event->error_message.get()->set_value("Lobby or session not found");
          return;
        }
        logs::log(logs::info, "[LOBBY] Session {} joining lobby {}", session->session_id, lobby->id);

        auto current_lobby = state::get_lobby_by_connected_session(lobbies.get(), std::to_string(session->session_id));
        if (current_lobby && current_lobby->id == lobby->id) {
          logs::log(logs::debug, "[LOBBY] Session {} is already in lobby {}", session->session_id, lobby->id);
          join_lobby_event->error_message.get()->set_value("");
          return;
        }

        if (!lobby->multi_user && lobby->connected_sessions->load()->size() >= 1) {
          logs::log(logs::error, "[LOBBY] Lobby {} is full", lobby->id);
          join_lobby_event->error_message.get()->set_value("Lobby is full");
          return;
        }

        // A session is in one lobby at most: a new stream resolves its producers to the first lobby that lists it
        // (state::stream_producer_name), while a running one follows the last switch
        if (current_lobby) {
          leave_lobby(app_state->event_bus, *current_lobby, *session);
        }

        // Migrate joypads BEFORE adding the session to connected_sessions, or the relayed unplug races the queued plug
        events::JoypadList joypads = session->joypads->load();
        // Joypads the lobby kept for this session: those its stream took over (sessions/add) are in `joypads`, plugged
        // into the lobby all along; any other is unplugged now
        release_parked_joypads(app_state->event_bus, *lobby, session->session_id, joypads);
        // The ones its stream took over are in a lobby's app, not in the stream's runner
        std::shared_ptr<const events::TakenOverJoypads> taken_over = session->taken_over_joypads->exchange(nullptr).get();
        for (auto [joypad_nr, joypad] : joypads) {
          auto taken = taken_over ? taken_over->joypads.find(joypad_nr) : nullptr;
          bool is_taken_over = taken && *taken == joypad;
          if (is_taken_over && taken_over->lobby_id == lobby->id) {
            continue; // already plugged into this lobby's app
          }
          events::PlugDeviceEvent plug_ev{.session_id = lobby->id};
          std::visit(
              [&plug_ev](auto &pad) {
                plug_ev.udev_events = pad.get_udev_events();
                plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
              },
              *joypad);
          // Unplug it from where it is: the current session's runner, or the app of the lobby it was taken over from
          app_state->event_bus->fire_event(immer::box<events::UnplugDeviceEvent>{events::UnplugDeviceEvent{
              .session_id = is_taken_over ? taken_over->lobby_id : std::to_string(session->session_id),
              .udev_events = plug_ev.udev_events,
              .udev_hw_db_entries = plug_ev.udev_hw_db_entries}});

          // Add it to the lobby runner's devices queue, addressed to the lobby id
          // (same routing the PlugDeviceEvent relay performs for connected sessions)
          lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>{plug_ev});
        }
        // TODO: hotplug pen_tablet

        // Update the lobby with the new session
        lobby->connected_sessions->update([session](const immer::vector<immer::box<std::string>> &connected_sessions) {
          return connected_sessions.push_back({std::to_string(session->session_id)});
        });

        // switch mouse and keyboard in session to use the lobby wayland server
        auto wl_state = lobby->wayland_display->load();
        session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
        session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
        session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));

        // Switch audio/video gstreamer stream producers
        app_state->event_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>{
            events::SwitchStreamProducerEvents{.session_id = session->session_id, .interpipe_src_id = lobby->id}});
        join_lobby_event->error_message.get()->set_value("");
      }));

  // When a Moonlight session leaves the lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::LeaveLobbyEvent>>(
      [=](const immer::box<events::LeaveLobbyEvent> &leave_lobby_event) {
        std::lock_guard membership_guard(*membership_lock);
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), leave_lobby_event->lobby_id);
        auto sessions = app_state->running_sessions->load();
        auto session = state::get_session_by_id(sessions.get(), leave_lobby_event->moonlight_session_id);

        if (!lobby || !session) {
          logs::log(logs::error,
                    "[LOBBY] Failed to leave lobby: lobby {} or session {} not found",
                    leave_lobby_event->lobby_id,
                    leave_lobby_event->moonlight_session_id);
        } else {
          leave_lobby(app_state->event_bus, lobby.value(), session.value());
        }
      }));

  // Stopping a lobby will trigger leave for all the connected sessions
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
      [=](const immer::box<events::StopLobbyEvent> &stop_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), stop_lobby_event->lobby_id);

        if (!lobby) {
          logs::log(logs::warning, "[LOBBY] lobby {} not found", stop_lobby_event->lobby_id);
          return;
        }
        logs::log(logs::info, "[LOBBY] stopping lobby {}", stop_lobby_event->lobby_id);

        if (!lobby->runner_claimed->exchange(true)) {
          // Its runner never started: remove the audio sink the runner would have removed on exit.
          // On another thread: an event handler must not block on PulseAudio.
          if (std::shared_ptr<audio::VSink> sink = lobby->audio_sink->exchange(nullptr);
              sink && audio_server && audio_server->server) {
            std::thread([server = audio_server->server, sink]() { audio::delete_virtual_sink(server, sink); }).detach();
          }
        }

        immer::vector<immer::box<std::string>> sessions = lobby->connected_sessions->load();
        for (auto &session_id : sessions) {
          app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>{
              events::LeaveLobbyEvent{.lobby_id = lobby->id, .moonlight_session_id = std::stoul(*session_id)}});
        }
        auto parked = lobby->parked_joypads->load();
        for (auto [session_id, _joypads] : *parked) {
          release_parked_joypads(app_state->event_bus, *lobby, session_id);
        }

        // Finally, remove the lobby from the app_state
        app_state->lobbies->update([stop_lobby_event](const immer::vector<events::Lobby> &lobbies) {
          return lobbies | //
                 ranges::views::filter([stop_lobby_event](const events::Lobby &lobby) {
                   return lobby.id != stop_lobby_event->lobby_id;
                 }) | //
                 ranges::to<immer::vector<events::Lobby>>();
        });
      }));

  // On a PlugDeviceEvent, we have to add the device to the lobby queue so that the runner will pick it up
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [=](const immer::box<events::PlugDeviceEvent> &plug_device_event) {
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        if (auto lobby = state::get_lobby_by_connected_session(lobbies, plug_device_event->session_id)) {
          logs::log(logs::info,
                    "[LOBBY] adding device to session {} in lobby {}",
                    plug_device_event->session_id,
                    lobby->id);

          lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>{
              events::PlugDeviceEvent{.session_id = lobby->id,
                                      .udev_events = plug_device_event->udev_events,
                                      .udev_hw_db_entries = plug_device_event->udev_hw_db_entries}});
        }
      }));

  // When a device is unplugged from a Moonlight session, we have to re-fire the event on our lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
      [=](const immer::box<events::UnplugDeviceEvent> &unplug_device_event) {
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        if (auto lobby = state::get_lobby_by_connected_session(lobbies, unplug_device_event->session_id)) {
          logs::log(logs::debug, "[LOBBY] Unplug device for session {}", unplug_device_event->session_id);
          app_state->event_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
              events::UnplugDeviceEvent{.session_id = lobby->id,
                                        .udev_events = unplug_device_event->udev_events,
                                        .udev_hw_db_entries = unplug_device_event->udev_hw_db_entries}});
        }
      }));

  // `stream_over`: the stream ended (paused or stopped), rather than its user going back to WolfUI
  auto on_moonlight_session_over = [app_state](std::size_t moonlight_session_id, bool stream_over) {
    immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
    if (auto lobby = state::get_lobby_by_connected_session(lobbies, std::to_string(moonlight_session_id))) {
      logs::log(logs::info, "[LOBBY] Moonlight stream {} over, leaving lobby {}", moonlight_session_id, lobby->id);
      if (stream_over && app_state->host->lobby_keeps_joypads) {
        // Before the leave, which then leaves them plugged into the lobby (leave_lobby)
        auto sessions = app_state->running_sessions->load();
        if (auto session = state::get_session_by_id(sessions.get(), moonlight_session_id)) {
          if (events::JoypadList joypads = session->joypads->load(); !joypads.empty()) {
            auto parked = std::make_shared<const events::ParkedJoypads>(
                events::ParkedJoypads{.joypads = joypads, .control_feedback = session->control_feedback});
            lobby->parked_joypads->update(
                [&](const auto &all) { return all.set(moonlight_session_id, std::move(parked)); });
            logs::log(logs::info,
                      "[LOBBY] Keeping {} joypad(s) of session {} plugged into lobby {} for its next stream",
                      joypads.size(),
                      moonlight_session_id,
                      lobby->id);
          }
        }
      }
      // Fire the LeaveLobbyEvent so that it can also be picked up by WolfUI via SSE
      app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>{
          events::LeaveLobbyEvent{.lobby_id = lobby->id, .moonlight_session_id = moonlight_session_id}});
    }
  };

  // When a Moonlight client Pauses a session, we get the user out of a lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
      [=](const immer::box<events::PauseStreamEvent> &pause_stream_event) {
        on_moonlight_session_over(pause_stream_event->session_id, true);
      }));

  // When a Moonlight client Stops a session, we get the user out of a lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopStreamEvent>>(
      [=](const immer::box<events::StopStreamEvent> &stop_stream_event) {
        on_moonlight_session_over(stop_stream_event->session_id, true);
      }));

  // When a client presses the WolfUI special combo, we get the user out of the lobby (and back to WolfUI)
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::ClientWolfUIComboEvent>>(
      [=](const immer::box<events::ClientWolfUIComboEvent> &event) {
        if (!app_state->host->wolf_ui_combo) {
          logs::log(logs::debug, "Ignoring the WolfUI combo for session {}: no WolfUI", event->session_id);
          return;
        }
        logs::log(logs::info, "Detected WolfUI combo for session {}", event->session_id);
        on_moonlight_session_over(event->session_id, false);
      }));

  return handlers.persistent();
}

} // namespace wolf::core::sessions
