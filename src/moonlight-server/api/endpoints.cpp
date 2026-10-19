#include <api/api.hpp>
#include <control/input_handler.hpp>
#include <core/docker.hpp>
#include <rtp/udp-ping.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>

namespace wolf::api {

/**
 * The reply to a change that was applied but couldn't be saved to config.toml: it is lost on the next restart
 */
static std::string not_saved_error() {
  return rfl::json::write(
      GenericErrorResponse{.error = "Applied, but config.toml couldn't be saved: the change is lost on restart"});
}

/**
 * Moonlight's rikeyid: a (possibly negative) decimal, see is_rikeyid() in rest/endpoints.hpp
 */
static bool is_aes_iv(std::string_view str) {
  std::int64_t value = 0;
  auto [end, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
  return ec == std::errc{} && end == str.data() + str.size();
}

/**
 * Moonlight's rikey: an AES-128 key, as 32 hex digits
 */
static bool is_aes_key(std::string_view str) {
  return str.size() == 32 && std::ranges::all_of(str, [](unsigned char c) { return std::isxdigit(c); });
}

void UnixSocketServer::endpoint_Events(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  // curl -N --unix-socket /tmp/wolf.sock http://localhost/api/v1/events
  send_http(socket,
            200,
            {{"Content-Type: text/event-stream"}, {"Connection: keep-alive"}, {"Cache-Control: no-cache"}},
            ""); // Inform clients this is going to be SS
  // Endpoints run on the HTTP thread pool, the list of sockets belongs to the io_context thread.
  // Registered after queueing the headers, so that no event can be sent before them.
  boost::asio::dispatch(state_->io_context, [this, socket]() { state_->sockets.push_back(socket); });
}

void UnixSocketServer::endpoint_PendingPairRequest(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto requests = std::vector<PendingPairClient>();
  auto pairing = state_->app_state->pairing_atom->load(); // iterate a snapshot that an update can't free under us
  for (auto [secret, pair_request] : *pairing) {
    requests.push_back({.pair_secret = secret, .client_ip = pair_request->client_ip});
  }
  send_http(socket, 200, rfl::json::write(PendingPairRequestsResponse{.requests = requests}));
}

void UnixSocketServer::endpoint_Pair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<PairRequest>(req.body);
  if (event) {
    auto pairing = state_->app_state->pairing_atom->load(); // find() points into this snapshot: keep it alive
    if (auto pair_request = pairing->find(event.value().pair_secret)) {
      pair_request->get().user_pin->set_value(event.value().pin.value()); // Resolve the promise
      state_->app_state->pairing_atom->update(
          [pair_secret = event.value().pair_secret](auto pairing_map) { return pairing_map.erase(pair_secret); });
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid pair secret: {}", event.value().pair_secret);
      auto res = GenericErrorResponse{.error = "Invalid pair secret"};
      send_http(socket, 404, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 400, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_PairedClients(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = PairedClientsResponse{.success = true};
  auto clients = state_->app_state->config->paired_clients->load();
  for (const config::PairedClient &client : clients.get()) {
    res.clients.push_back(PairedClient{.client_id = std::to_string(state::get_client_id(client)),
                                       .app_state_folder = client.app_state_folder,
                                       .settings = client.settings});
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_UnpairClient(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  try {
    auto payload_result = rfl::json::read<UnpairClientRequest>(req.body);
    if (!payload_result) {
      auto res = GenericErrorResponse{.error = "Invalid request format"};
      send_http(socket, 400, rfl::json::write(res));
      return;
    }

    const auto &payload = payload_result.value(); // Unwrap the Result
    if (!utils::to_size_t(payload.client_id.value())) {
      send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid client_id"}));
      return;
    }
    auto client = state::get_client_by_id(this->state_->app_state->config, payload.client_id.value());
    if (!client) {
      auto res = GenericErrorResponse{.error = "Client not found"};
      send_http(socket, 404, rfl::json::write(res));
      return;
    }

    if (!state::unpair(this->state_->app_state->config, *client)) {
      send_http(socket, 500, not_saved_error());
      return;
    }

    auto res = GenericSuccessResponse{.success = true};
    send_http(socket, 200, rfl::json::write(res));
  } catch (const std::exception &e) {
    auto res = GenericErrorResponse{.error = e.what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Apps(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = AppListResponse{.success = true};
  auto moonlight_profile = state::get_moonlight_profile(state_->app_state->config);
  if (!moonlight_profile) {
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Moonlight profile not found"}));
    return;
  }
  immer::vector<immer::box<events::App>> app_list = moonlight_profile.value()->apps->load();
  for (const immer::box<events::App> &app : app_list) {
    res.apps.push_back(rfl::Reflector<events::App>::from(app));
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<rfl::Reflector<events::App>::ReflType>(req.body);
  if (app) {
    bool duplicate = false;
    auto saved = state::update_profiles(
        state_->app_state->config,
        [app = app.value(), &duplicate, this](const state::ProfilesList &profiles) {
          return profiles | //
                 ranges::views::transform([&app, &duplicate, this](const immer::box<events::Profile> &profile) {
                   if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                     profile->apps->update([&app, &duplicate, this](auto &apps) {
                       // A client retrying an add (e.g. after a failed save) must not create the app twice
                       duplicate = std::any_of(apps.begin(), apps.end(), [&app](const immer::box<events::App> &a) {
                         return a->base.id == app.id;
                       });
                       return duplicate ? apps
                                        : apps.push_back(
                                              rfl::Reflector<events::App>::to(app, this->state_->app_state->event_bus));
                     });
                   }
                   return profile;
                 }) |
                 ranges::to<state::ProfilesList>();
        });

    if (duplicate) {
      logs::log(logs::warning, "[API] App id already in use: {}", app.value().id);
      send_http(socket, 409, rfl::json::write(GenericErrorResponse{.error = "An app with this id already exists"}));
      return;
    }
    if (!saved) {
      send_http(socket, 500, not_saved_error());
      return;
    }
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 400, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<AppDeleteRequest>(req.body);
  if (app) {
    auto saved =
        state::update_profiles(state_->app_state->config, [app = app.value()](const state::ProfilesList &profiles) {
          return profiles | //
                 ranges::views::transform([&app](const immer::box<events::Profile> &profile) {
                   if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                     profile->apps->update([&app](auto &apps) {
                       return apps | //
                              ranges::views::filter(
                                  [&app](const immer::box<events::App> &a) { return a->base.id != app.id; }) | //
                              ranges::to<immer::vector<immer::box<events::App>>>();
                     });
                   }
                   return profile;
                 }) |
                 ranges::to<state::ProfilesList>();
        });

    if (!saved) {
      send_http(socket, 500, not_saved_error());
      return;
    }
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 400, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Profiles(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profiles = state_->app_state->config->profiles->load().get();
  auto res = ProfileListResponse{.success = true,
                                 .profiles = profiles | //
                                             ranges::views::filter([](const immer::box<events::Profile> &p) {
                                               return p->id != events::MOONLIGHT_PROFILE_ID;
                                             }) |                                                              //
                                             ranges::views::transform(rfl::Reflector<events::Profile>::from) | //
                                             ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<rfl::Reflector<events::Profile>::ReflType>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto saved = state::update_profiles(state_->app_state->config, [&p, this](const state::ProfilesList &profiles) {
      return profiles.push_back(rfl::Reflector<events::Profile>::to(p, this->state_->app_state->event_bus));
    });
    if (!saved) {
      send_http(socket, 500, not_saved_error());
      return;
    }
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 400, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<ProfileRemoveRequest>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto saved = state::update_profiles(state_->app_state->config, [&p](const state::ProfilesList &profiles) {
      return profiles | //
             ranges::views::remove_if(
                 [&p](const immer::box<events::Profile> &profile) { return profile.get().id == p.id; }) | //
             ranges::to<state::ProfilesList>();
    });
    if (!saved) {
      send_http(socket, 500, not_saved_error());
      return;
    }
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 400, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessions(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = StreamSessionListResponse{.success = true};
  auto sessions = state_->app_state->running_sessions->load();
  for (const auto &session : sessions.get()) {
    auto stream_state = session.stream_state->load();
    auto state = StreamStateName::make<"setup">();
    if (stream_state->paused) {
      state = StreamStateName::make<"paused">();
    } else if (stream_state->video_pipelines > 0 && stream_state->audio_pipelines > 0) {
      state = StreamStateName::make<"streaming">();
    }
    res.sessions.push_back({.session = rfl::Reflector<events::StreamSession>::from(session), .state = state});
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_StreamSessionAdd(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<rfl::Reflector<events::StreamSession>::ReflType>(req.body);
  if (session) {
    immer::box<events::App> choosen_app;
    auto ss = session.value();
    if (!moonlight::is_valid_display_mode(ss.video_width, ss.video_height, ss.video_refresh_rate)) {
      logs::log(logs::warning,
                "[API] Invalid display mode: {}x{}x{}",
                ss.video_width,
                ss.video_height,
                ss.video_refresh_rate);
      auto res = GenericErrorResponse{.error = "Invalid video_width, video_height or video_refresh_rate"};
      send_http(socket, 400, rfl::json::write(res));
      return;
    }
    // The audio of every stream of this session is encrypted with them: the payloader can't run with a missing or
    // short key, and derive_iv() reads a non-decimal IV as 0, which the client couldn't decrypt
    if (!is_aes_key(ss.aes_key)) {
      logs::log(logs::warning, "[API] Invalid aes_key");
      send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid aes_key, expects 32 hex digits"}));
      return;
    }
    if (!is_aes_iv(ss.aes_iv)) {
      logs::log(logs::warning, "[API] Invalid aes_iv: {}", ss.aes_iv);
      send_http(socket,
                400,
                rfl::json::write(GenericErrorResponse{.error = "Invalid aes_iv, expects a decimal number"}));
      return;
    }
    if (auto app_id = ss.app_id) {
      auto app = state::get_moonlight_app_by_id(this->state_->app_state->config, *app_id);
      if (!app) {
        logs::log(logs::warning, "[API] Invalid app_id: {}", *app_id);
        auto res = GenericErrorResponse{.error = "Invalid app_id"};
        send_http(socket, 404, rfl::json::write(res));
        return;
      }
      choosen_app = *app;
    } else {
      auto moonlight_profile = state::get_moonlight_profile(this->state_->app_state->config);
      if (!moonlight_profile) {
        logs::log(logs::warning, "[API] No moonlight profile found, unable to automatically create an app.");
        auto res = GenericErrorResponse{.error = "No moonlight profile found"};
        send_http(socket, 500, rfl::json::write(res));
        return;
      }
      immer::vector<immer::box<events::App>> apps = moonlight_profile.value()->apps->load();
      immer::box<events::App> sample_app = apps.front();
      choosen_app = events::App{
          .base = {.title = "dummy", .id = state::gen_uuid(), .support_hdr = false, .icon_png_path = ""},

          .video_producer_buffer_caps = sample_app->video_producer_buffer_caps,

          .h264_gst_pipeline = sample_app->h264_gst_pipeline,
          .hevc_gst_pipeline = sample_app->hevc_gst_pipeline,
          .av1_gst_pipeline = sample_app->av1_gst_pipeline,

          .render_node = sample_app->render_node,
          .opus_gst_pipeline = sample_app->opus_gst_pipeline,
          .start_virtual_compositor = true,
          .start_audio_server = true,

          .runner = std::make_shared<process::RunProcess>(state_->app_state->event_bus,
                                                          "sh -c \"while :; do echo 'running...'; sleep 10; done\"")};
    }

    config::PairedClient choosen_client;
    if (auto client_id = ss.client_id) {
      if (!utils::to_size_t(*client_id)) {
        logs::log(logs::warning, "[API] Invalid client_id: {}", *client_id);
        send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid client_id"}));
        return;
      }
      auto client = state::get_client_by_id(this->state_->app_state->config, *client_id);
      if (!client) {
        logs::log(logs::warning, "[API] Invalid client_id: {}", *client_id);
        auto res = GenericErrorResponse{.error = "Invalid client_id"};
        send_http(socket, 404, rfl::json::write(res));
        return;
      }
      choosen_client = *client;
    } else {
      // Create a dummy client. The session_id is the hash of its cert: a unique one keeps two such sessions apart
      choosen_client = {.client_cert = state::gen_uuid(), .app_state_folder = state::gen_uuid(), .settings = {}};
    }

    choosen_client.settings = ss.client_settings.value_or(config::ClientSettings{});

    auto new_session = state::create_stream_session( //
        state_->app_state,
        choosen_app,
        choosen_client,
        moonlight::DisplayMode{.width = ss.video_width,
                               .height = ss.video_height,
                               .refreshRate = ss.video_refresh_rate,
                               .hevc_supported = state_->app_state->config->support_hevc,
                               .av1_supported = state_->app_state->config->support_av1},
        ss.audio_channel_count,
        ss.aes_key,
        ss.aes_iv);
    new_session->ip = ss.client_ip;
    new_session->rtsp_fake_ip = ss.rtsp_fake_ip;

    // Every session event is keyed by session_id: a second session under the same one would be unreachable.
    // Checked inside the update, so that concurrent adds for one client can't all pass the check.
    bool already_running = false;
    state_->app_state->running_sessions->update(
        [new_session, &already_running](const immer::vector<events::StreamSession> &ses_v) {
          already_running = state::get_session_by_id(ses_v, new_session->session_id).has_value();
          return already_running ? ses_v : ses_v.push_back(*new_session);
        });
    if (already_running) {
      logs::log(logs::warning, "[API] Client already has a running session: {}", new_session->session_id);
      auto res = GenericErrorResponse{.error = "Client already has a running session, stop it first"};
      send_http(socket, 409, rfl::json::write(res));
      return;
    }
    state_->app_state->event_bus->fire_event(immer::box<events::StreamSession>(*new_session));

    auto res = StreamSessionCreated{.success = true, .session_id = std::to_string(new_session->session_id)};
    send_http(socket, 200, rfl::json::write(res));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 400, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto start_req = rfl::json::read<StreamSessionStartRequest>(req.body);
  if (start_req) {
    auto sessions = state_->app_state->running_sessions->load();
    auto parsed_session_id = utils::to_size_t(start_req.value().session_id);
    if (!parsed_session_id) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", start_req.value().session_id);
      send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
      return;
    }
    auto session_id = *parsed_session_id;
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto video_session = start_req.value().video_session;
      auto mode = video_session.display_mode;
      if (!moonlight::is_valid_display_mode(mode.width, mode.height, mode.refreshRate)) {
        logs::log(logs::warning, "[API] Invalid display mode: {}x{}x{}", mode.width, mode.height, mode.refreshRate);
        auto res = GenericErrorResponse{.error = "Invalid video_session.display_mode"};
        send_http(socket, 400, rfl::json::write(res));
        return;
      }
      const auto &audio = start_req.value().audio_session;
      if (audio.encrypt_audio && (!is_aes_key(audio.aes_key) || !is_aes_iv(audio.aes_iv))) {
        logs::log(logs::warning, "[API] Invalid audio_session aes_key or aes_iv");
        auto res = GenericErrorResponse{
            .error = "Invalid audio_session.aes_key or aes_iv, expects 32 hex digits and a decimal number"};
        send_http(socket, 400, rfl::json::write(res));
        return;
      }
      video_session.session_id = session_id; // Can't be JSON encoded
      if (video_session.render_node.empty()) {
        video_session.render_node = session->app->render_node;
      }
      state_->app_state->event_bus->fire_event(immer::box<events::VideoSession>(video_session));

      auto audio_session = start_req.value().audio_session;
      audio_session.session_id = session_id; // Can't be JSON encoded
      state_->app_state->event_bus->fire_event(immer::box<events::AudioSession>(audio_session));

      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", start_req.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 404, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, start_req.error().what());
    auto res = GenericErrorResponse{.error = start_req.error().what()};
    send_http(socket, 400, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionPause(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionPauseRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    auto parsed_session_id = utils::to_size_t(session.value().session_id);
    if (!parsed_session_id) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
      return;
    }
    auto session_id = *parsed_session_id;
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::PauseStreamEvent>(events::PauseStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 404, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 400, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionStopRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    auto parsed_session_id = utils::to_size_t(session.value().session_id);
    if (!parsed_session_id) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
      return;
    }
    auto session_id = *parsed_session_id;
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
      return;
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 404, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 400, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionHandleInput(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_request = rfl::json::read<StreamSessionHandleInputRequest>(req.body);
  if (input_request) {
    auto sessions = state_->app_state->running_sessions->load();
    auto parsed_session_id = utils::to_size_t(input_request.value().session_id);
    if (!parsed_session_id) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", input_request.value().session_id);
      send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
      return;
    }
    auto session_id = *parsed_session_id;
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto hex_pkt = input_request.value().input_packet_hex.get();
      auto pkt_parsed = crypto::hex_to_str(hex_pkt);
      control::INPUT_PKT *input_pkt = reinterpret_cast<control::INPUT_PKT *>(pkt_parsed.data());
      control::handle_input(session.value(), {}, input_pkt, pkt_parsed.size());

      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", input_request.value().session_id);
      send_http(socket, 404, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, input_request.error().what());
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = input_request.error().what()}));
  }
}

void UnixSocketServer::endpoint_Lobbies(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  immer::vector<events::Lobby> lobbies = state_->app_state->lobbies->load();
  auto res = LobbiesResponse{.lobbies = lobbies | //
                                        ranges::views::transform([](const events::Lobby &lobby) {
                                          return rfl::Reflector<events::Lobby>::from(lobby);
                                        }) | //
                                        ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_LobbyCreate(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<CreateLobbyRequest>(req.body);
  if (event) {
    const auto &video = event.value().video_settings;
    if (!moonlight::is_valid_display_mode(video.width, video.height, video.refresh_rate)) {
      logs::log(logs::warning,
                "[API] Invalid lobby display mode: {}x{}x{}",
                video.width,
                video.height,
                video.refresh_rate);
      auto res = GenericErrorResponse{.error = "Invalid video_settings width, height or refresh_rate"};
      send_http(socket, 400, rfl::json::write(res));
      return;
    }
    auto video_settings = event.value().video_settings;
    // A blank caps string would produce an invalid producer pipeline, treat it like a missing one (as apps do)
    if (std::ranges::all_of(video_settings.video_producer_buffer_caps,
                            [](unsigned char c) { return std::isspace(c); })) {
      video_settings.video_producer_buffer_caps = "video/x-raw";
    }
    auto default_client_settings = state::ClientSettings{};
    auto client_settings = event.value().client_settings.value().value_or(PartialClientSettings{});
    auto lobby_id = state::gen_uuid();
    auto create_lobby_ev = events::CreateLobbyEvent{
        .id = lobby_id,
        .profile_id = event.value().profile_id.get(),
        .name = event.value().name,
        .icon_png_path = event.value().icon_png_path,
        .pin = event.value().pin.get(),
        .multi_user = event.value().multi_user,
        .stop_when_everyone_leaves = event.value().stop_when_everyone_leaves,
        .video_settings = video_settings,
        .audio_settings = event.value().audio_settings,
        .client_settings =
            state::ClientSettings{
                .run_uid = client_settings.run_uid.value_or(default_client_settings.run_uid),
                .run_gid = client_settings.run_gid.value_or(default_client_settings.run_gid),
                .controllers_override =
                    client_settings.controllers_override.value_or(default_client_settings.controllers_override),
                .mouse_acceleration =
                    client_settings.mouse_acceleration.value_or(default_client_settings.mouse_acceleration),
                .v_scroll_acceleration =
                    client_settings.v_scroll_acceleration.value_or(default_client_settings.v_scroll_acceleration),
                .h_scroll_acceleration =
                    client_settings.h_scroll_acceleration.value_or(default_client_settings.h_scroll_acceleration),
                .motion_controller_override = client_settings.motion_controller_override.value_or(
                    default_client_settings.motion_controller_override)},
        .runner_state_folder = event.value().runner_state_folder,
        .runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus)};
    // Fire the event
    state_->app_state->event_bus->fire_event(immer::box<events::CreateLobbyEvent>(create_lobby_ev));

    auto setup_over_future = create_lobby_ev.on_setup_over.get()->get_future();
    auto result = setup_over_future.wait_for(std::chrono::seconds(20));
    if (result == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby setup timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby setup timed out"}));
    } else {
      auto res = LobbyCreateResponse{.lobby_id = lobby_id};
      send_http(socket, 200, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

/**
 * The HTTP status for a lobby error message: the client asked for something that isn't there or isn't allowed
 */
static int lobby_error_status(std::string_view error) {
  if (error == "Invalid PIN") {
    return 403;
  } else if (error == "Lobby is full") {
    return 409;
  }
  return 404; // "Invalid lobby ID", "Lobby or session not found"
}

std::optional<std::string /* Error message */> check_lobby_pin(const immer::vector<events::Lobby> &lobbies,
                                                               std::string_view lobby_id,
                                                               const std::optional<std::vector<short>> &pin) {
  auto lobby = state::get_lobby_by_id(lobbies, lobby_id);
  if (!lobby) {
    return "Invalid lobby ID";
  }
  if (lobby->pin != pin) {
    return "Invalid PIN";
  }
  return std::nullopt;
}

void UnixSocketServer::endpoint_LobbyJoin(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::JoinLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, lobby_error_status(*err), rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    auto lobby_ev = event.value();
    lobby_ev.error_message = std::make_shared<std::promise<std::string>>();
    state_->app_state->event_bus->fire_event(immer::box<events::JoinLobbyEvent>(lobby_ev));

    auto error_message_fut = lobby_ev.error_message.get()->get_future();
    auto future_status = error_message_fut.wait_for(std::chrono::seconds(2));
    if (future_status == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby join timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby join timed out"}));
    } else if (auto error_message = error_message_fut.get(); !error_message.empty()) {
      logs::log(logs::warning, "[API] Lobby join failed: {}", error_message);
      send_http(socket,
                lobby_error_status(error_message),
                rfl::json::write(GenericErrorResponse{.error = utils::to_string(error_message)}));
    } else {
      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyLeave(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::LeaveLobbyEvent>(req.body);
  if (event) {
    state_->app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyStop(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::StopLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, lobby_error_status(*err), rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    state_->app_state->event_bus->fire_event(immer::box<events::StopLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_RunnerStart(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<RunnerStartRequest>(req.body);
  if (event) {
    auto session_id = utils::to_size_t(event.value().session_id);
    if (!session_id) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", event.value().session_id);
      send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
      return;
    }
    auto session = state::get_session_by_id(this->state_->app_state->running_sessions->load(), *session_id);
    if (!session) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", event.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 404, rfl::json::write(res));
      return;
    }

    auto runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus);
    state_->app_state->event_bus->fire_event(immer::box<events::StartRunner>(
        events::StartRunner{.stop_stream_when_over = event.value().stop_stream_when_over,
                            .runner = runner,
                            .stream_session = std::make_shared<events::StreamSession>(*session)}));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 400, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_UpdateClientSettings(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload_result = rfl::json::read<UpdateClientSettingsRequest>(req.body);
  if (!payload_result) {
    auto res = GenericErrorResponse{.error = "Invalid request format"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  const auto &payload = payload_result.value();
  auto client_id = utils::to_size_t(payload.client_id.value());
  if (!client_id) {
    auto res = GenericErrorResponse{.error = "Invalid client_id"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  // Edit only the settings that are being passed in the payload, merged into the client as it is when applied
  auto merge_fn = [&payload](const config::PairedClient &current_client) {
    auto current_settings = current_client.settings;
    auto new_settings = payload.settings.get().value_or(PartialClientSettings{});
    return config::PairedClient{
        .client_cert = current_client.client_cert, // Immutable, changing this would mean a new client
        .app_state_folder = payload.app_state_folder.get().value_or(current_client.app_state_folder),
        .settings = config::ClientSettings{
            .run_uid = new_settings.run_uid.value_or(current_settings.run_uid),
            .run_gid = new_settings.run_gid.value_or(current_settings.run_gid),
            .controllers_override = new_settings.controllers_override.value_or(current_settings.controllers_override),
            .mouse_acceleration = new_settings.mouse_acceleration.value_or(current_settings.mouse_acceleration),
            .v_scroll_acceleration =
                new_settings.v_scroll_acceleration.value_or(current_settings.v_scroll_acceleration),
            .h_scroll_acceleration =
                new_settings.h_scroll_acceleration.value_or(current_settings.h_scroll_acceleration),
            .motion_controller_override =
                new_settings.motion_controller_override.value_or(current_settings.motion_controller_override),
        }};
  };

  auto update = update_client_settings(this->state_->app_state->config, *client_id, merge_fn);
  if (update == state::ConfigUpdate::NOT_FOUND) {
    auto res = GenericErrorResponse{.error = "Client not found"};
    send_http(socket, 404, rfl::json::write(res));
    return;
  } else if (update == state::ConfigUpdate::NOT_SAVED) {
    send_http(socket, 500, not_saved_error());
    return;
  }

  auto res = GenericSuccessResponse{.success = true};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_GetIcon(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto icon_path = utils::split(req.query_string, '=');
  if (icon_path.size() != 2 || icon_path[0] != "icon_path") {
    auto res = GenericErrorResponse{.error = "Invalid request format, expects 'icon_path' as a query parameter"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }
  // TODO: implement coroutines for CURL
  std::thread([this, socket, icon_path = utils::to_string(icon_path[1])]() {
    if (auto icon = utils::get_icon(this->state_->app_state->host->local_base_state_folder, icon_path)) {
      send_http(socket,
                200,
                {"Content-Length: " + std::to_string(icon->size()), "Content-Type: image/png"},
                icon.value());
    } else {
      auto res = GenericErrorResponse{.error = "Icon not found"};
      send_http(socket, 404, rfl::json::write(res));
    }
  }).detach();
}

void UnixSocketServer::endpoint_DockerInspectImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto image_name = utils::split(req.query_string, '=');
  if (image_name.size() != 2 || image_name[0] != "image_name") {
    auto res = GenericErrorResponse{.error = "Invalid request format, expects 'image_name' as a query parameter"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
  if (auto response = docker_api.inspect_image(image_name[1])) {
    send_http(socket, 200, response.value());
  } else {
    auto res = GenericErrorResponse{.error = "Image not found"};
    send_http(socket, 404, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_DockerPullImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_payload = rfl::json::read<DockerPullImageRequest>(req.body);
  if (!input_payload) {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, input_payload.error().what());
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = input_payload.error().what()}));
    return;
  }
  if (std::ranges::all_of(input_payload.value().image_name, [](unsigned char c) { return std::isspace(c); })) {
    send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Missing image_name"}));
    return;
  }
  if (input_payload) {
    // TODO: implement coroutines for CURL
    std::thread([this, socket, image = input_payload.value().image_name]() {
      docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
      bool first_send = true;
      broadcast_event("DockerPullImageStartEvent",
                      rfl::json::write(events::DockerPullImageStartEvent{.image_name = image}));
      if (docker_api.pull_image(image,
                                {},
                                [this, &first_send, socket](const docker::DockerAPI::DockerProgressEvent &progress_ev) {
                                  if (first_send) {
                                    send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
                                    first_send = false;
                                  }
                                  auto serialized_ev = rfl::json::write(progress_ev) + "\r\n";
                                  send_data(socket, serialized_ev);
                                })) {
        if (first_send) {
          send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
        }
        auto final_result = rfl::json::write(GenericSuccessResponse{.success = true});
        send_data(socket, final_result + "\r\n");
        broadcast_event("DockerPullImageEndEvent",
                        rfl::json::write(events::DockerPullImageEndEvent{.image_name = image, .success = true}));
      } else {
        auto error = rfl::json::write(GenericErrorResponse{.error = "Failed to pull image"});
        if (first_send) {
          send_http(socket, 500, error);
        } else { // The 200 header and the progress already went out: report the failure as the final line
          send_data(socket, error + "\r\n");
        }
        broadcast_event("DockerPullImageEndEvent",
                        rfl::json::write(events::DockerPullImageEndEvent{.image_name = image, .success = false}));
      }
    }).detach();
  }
}

} // namespace wolf::api