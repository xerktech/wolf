#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/audio.hpp>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <future>
#include <helpers/utils.hpp>
#include <mutex>
#include <pulse/pulseaudio.h>
#include <sessions/common.hpp>
#include <sessions/handlers.hpp>
#include <set>
#include <state/config.hpp>
#include <thread>
#include <vector>

using namespace wolf::core;
using namespace std::chrono_literals;

namespace {

/**
 * A PulseAudio daemon of our own, on a private socket: tests can freeze or kill it without touching anybody else's.
 */
struct PrivatePulse {
  std::filesystem::path dir;
  std::string socket;
  pid_t pid = 0;

  PrivatePulse() {
    char tmpl[] = "/tmp/wolf-pulse-XXXXXX";
    REQUIRE(mkdtemp(tmpl));
    dir = tmpl;
    socket = (dir / "native").string();
    auto cmd = fmt::format("HOME={0} XDG_RUNTIME_DIR={0} pulseaudio -D -n --exit-idle-time=-1 --use-pid-file=yes "
                           "--load=\"module-native-protocol-unix socket={1} auth-anonymous=1\" "
                           "--load=module-null-sink --log-target=stderr 2>/dev/null",
                           dir.string(),
                           socket);
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::ifstream pid_file(dir / "pulse" / "pid");
    REQUIRE(pid_file >> pid);
  }

  ~PrivatePulse() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  void kill() {
    ::kill(pid, SIGKILL);
    pid = 0;
  }
};

/**
 * Never freed, like Wolf's own: an audio::Server doesn't stop its mainloop thread when it's destroyed, so freeing one
 * (e.g. when a failed REQUIRE unwinds a test) would leave that thread running on freed memory.
 */
std::shared_ptr<audio::Server> keep_alive(std::shared_ptr<audio::Server> server) {
  static std::mutex m;
  static std::vector<std::shared_ptr<audio::Server>> servers;
  std::lock_guard lock(m);
  servers.push_back(server);
  return server;
}

std::shared_ptr<audio::Server> connect(const PrivatePulse &pulse) {
  auto server = keep_alive(audio::connect(pulse.socket));
  REQUIRE(audio::connected(server));
  return server;
}

/**
 * Waits until PulseAudio has also handled what a load/unload callback sent in turn (each flush covers what was sent
 * before it).
 */
void settle(const std::shared_ptr<audio::Server> &server) {
  REQUIRE(audio::flush(server, 2s));
  REQUIRE(audio::flush(server, 2s));
}

bool sink_exists(const std::shared_ptr<audio::Server> &server, const std::string &name) {
  struct Query {
    std::string name;
    bool found = false;
    std::promise<bool> result;
  };
  auto query = new Query{.name = name};
  auto result = query->result.get_future();
  audio::queue_op(server, [server, query]() {
    auto op = pa_context_get_sink_info_list(
        audio::context(server),
        [](pa_context *, const pa_sink_info *info, int eol, void *data) {
          auto query = static_cast<Query *>(data);
          if (eol) {
            query->result.set_value(query->found);
            delete query;
          } else if (info && info->name && query->name == info->name) {
            query->found = true;
          }
        },
        query);
    if (op) {
      pa_operation_unref(op);
    }
  });
  REQUIRE(result.wait_for(2s) == std::future_status::ready);
  return result.get();
}

/** Record streams on the server: a running audio producer's pulsesrc is one */
int source_outputs(const std::shared_ptr<audio::Server> &server) {
  struct Query {
    int count = 0;
    std::promise<int> result;
  };
  auto query = new Query{};
  auto result = query->result.get_future();
  audio::queue_op(server, [server, query]() {
    auto op = pa_context_get_source_output_info_list(
        audio::context(server),
        [](pa_context *, const pa_source_output_info *info, int eol, void *data) {
          auto query = static_cast<Query *>(data);
          if (eol) {
            query->result.set_value(query->count);
            delete query;
          } else if (info) {
            query->count++;
          }
        },
        query);
    if (op) {
      pa_operation_unref(op);
    }
  });
  REQUIRE(result.wait_for(2s) == std::future_status::ready);
  return result.get();
}

std::shared_ptr<audio::VSink> create_sink(const std::shared_ptr<audio::Server> &server, const std::string &name) {
  using Speakers = audio::AudioMode::Speakers;
  return audio::create_virtual_sink(
      server,
      {.sink_name = name, .mode = {.channels = 2, .speakers = {Speakers::FRONT_LEFT, Speakers::FRONT_RIGHT}}});
}

} // namespace

TEST_CASE("PulseAudio virtual sinks", "[PULSE]") {
  PrivatePulse pulse;
  auto server = connect(pulse);

  SECTION("create, then delete twice") {
    auto sink = create_sink(server, "wolf_test_sink");
    settle(server);
    REQUIRE(sink_exists(server, "wolf_test_sink"));

    audio::delete_virtual_sink(server, sink);
    audio::delete_virtual_sink(server, sink); // every runner of a session used to get here
    settle(server);
    REQUIRE_FALSE(sink_exists(server, "wolf_test_sink"));
  }

  SECTION("delete before the module finished loading") {
    auto sink = create_sink(server, "wolf_test_sink");
    audio::delete_virtual_sink(server, sink);
    sink.reset(); // the load callback must still have its own reference
    settle(server);
    REQUIRE_FALSE(sink_exists(server, "wolf_test_sink"));
  }

  SECTION("concurrent deletes") {
    auto sink = create_sink(server, "wolf_test_sink");
    settle(server);
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; i++) {
      threads.emplace_back([&]() { audio::delete_virtual_sink(server, sink); });
    }
    for (auto &t : threads) {
      t.join();
    }
    settle(server);
    REQUIRE_FALSE(sink_exists(server, "wolf_test_sink"));
  }

  SECTION("the sink name doesn't need to outlive the call") {
    {
      auto name = std::string("wolf_test_sink_temporary_name");
      create_sink(server, name);
    }
    settle(server);
    REQUIRE(sink_exists(server, "wolf_test_sink_temporary_name"));
  }

  SECTION("leftover sinks are replaced or removed") {
    auto reclaimed_name = fmt::format("{}1234", sessions::VIRTUAL_SINK_PREFIX);
    auto unclaimed_name = fmt::format("{}4321", sessions::VIRTUAL_SINK_PREFIX);
    { // A Wolf that crashed: its sinks outlive its connection
      auto dead_wolf = connect(pulse);
      create_sink(dead_wolf, reclaimed_name);
      create_sink(dead_wolf, unclaimed_name);
      settle(dead_wolf);
      audio::disconnect(dead_wolf);
    }
    auto other = create_sink(server, "wolf_test_not_ours");
    settle(server);

    REQUIRE(audio::find_leftover_virtual_sinks(server, sessions::VIRTUAL_SINK_PREFIX, 2s) == 2);

    // Its client relaunches right away (session ids are stable across restarts): the new sink must get the exact
    // name, which the app (PULSE_SINK) and Wolf's pulsesrc attach to; "<name>.2" left them on the leftover
    auto same_client = create_sink(server, reclaimed_name);
    auto live = create_sink(server, fmt::format("{}5678", sessions::VIRTUAL_SINK_PREFIX));
    // The reclaim loads only once PulseAudio reports the leftover gone (polled on a timer, which settle doesn't wait
    // for): wait for the load itself, or a slow CI runner checks before it happened
    REQUIRE(same_client->sink_idx_fut.wait_for(boost::chrono::seconds(3)) == boost::future_status::ready);
    REQUIRE(live->sink_idx_fut.wait_for(boost::chrono::seconds(3)) == boost::future_status::ready);
    settle(server);
    REQUIRE(sink_exists(server, reclaimed_name));
    REQUIRE_FALSE(sink_exists(server, fmt::format("{}.2", reclaimed_name)));

    // The sweep removes only the leftover nobody replaced
    audio::unload_leftover_virtual_sinks(server);
    settle(server);
    REQUIRE_FALSE(sink_exists(server, unclaimed_name));
    REQUIRE(sink_exists(server, reclaimed_name));
    REQUIRE(sink_exists(server, fmt::format("{}5678", sessions::VIRTUAL_SINK_PREFIX)));
    REQUIRE(sink_exists(server, "wolf_test_not_ours"));

    // ...and the sink under the reclaimed name is the new session's own, not the leftover
    audio::delete_virtual_sink(server, same_client);
    settle(server);
    // PulseAudio acks an unload before the sink is gone: give it a moment
    for (int i = 0; i < 100 && sink_exists(server, reclaimed_name); i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE_FALSE(sink_exists(server, reclaimed_name));
  }

  SECTION("many leftover sinks replaced at once") {
    constexpr int clients = 12;
    auto name = [](int i) { return fmt::format("{}{}", sessions::VIRTUAL_SINK_PREFIX, 9000 + i); };
    {
      auto dead_wolf = connect(pulse);
      for (int i = 0; i < clients; i++) {
        create_sink(dead_wolf, name(i));
      }
      settle(dead_wolf);
      audio::disconnect(dead_wolf);
    }
    REQUIRE(audio::find_leftover_virtual_sinks(server, sessions::VIRTUAL_SINK_PREFIX, 2s) == clients);

    // Every client relaunches at once: all must get their plain names. In a live Wolf, loading right after each
    // unload's ack got "<name>.2" now and then (PulseAudio acks before the module is gone); this test doesn't reproduce
    // that timing, it checks the wait for the name keeps every sink right
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<audio::VSink>> sinks(clients);
    for (int i = 0; i < clients; i++) {
      threads.emplace_back([&, i]() { sinks[i] = create_sink(server, name(i)); });
    }
    for (auto &t : threads) {
      t.join();
    }
    for (int i = 0; i < clients; i++) {
      REQUIRE(sinks[i]->sink_idx_fut.wait_for(boost::chrono::seconds(3)) == boost::future_status::ready);
    }
    settle(server);
    for (int i = 0; i < clients; i++) {
      INFO(name(i));
      REQUIRE(sink_exists(server, name(i)));
      REQUIRE_FALSE(sink_exists(server, fmt::format("{}.2", name(i))));
    }
  }

  audio::disconnect(server);
}

TEST_CASE("PulseAudio dying mid-request releases what the request held", "[PULSE]") {
  PrivatePulse pulse;
  auto server = connect(pulse);

  ::kill(pulse.pid, SIGSTOP); // requests from now on stay pending
  auto sink = create_sink(server, "wolf_test_sink");

  auto start = std::chrono::steady_clock::now();
  auto flushed = std::async(std::launch::async, [&]() { return audio::flush(server, 5s); });
  std::this_thread::sleep_for(300ms);
  pulse.kill();

  // libpulse cancels pending operations without calling their callbacks: flush used to wait out its whole timeout
  // and the sink's load request kept its VSink forever
  flushed.wait();
  REQUIRE(std::chrono::steady_clock::now() - start < 3s);
  for (int i = 0; i < 100 && sink.use_count() > 1; i++) {
    std::this_thread::sleep_for(20ms);
  }
  REQUIRE(sink.use_count() == 1);
}

namespace {
struct CaptureRunner : events::Runner {
  immer::array<std::pair<std::string, std::string>> paths;
  immer::map<std::string, std::string> env;
  std::atomic_bool ran = false;

  void run(std::string_view,
           std::string_view,
           std::string_view,
           std::shared_ptr<events::devices_atom_queue>,
           const immer::array<std::string> &,
           const immer::array<std::pair<std::string, std::string>> &paths,
           const immer::map<std::string, std::string> &env_variables,
           std::string_view,
           const std::function<bool()> &) override {
    this->paths = paths;
    this->env = env_variables;
    ran = true;
  }

  events::RunnerTypes serialize() const override {
    return wolf::config::AppCMD{.run_cmd = ""};
  }
};

struct BlockingRunner : events::Runner {
  std::shared_future<void> released;

  explicit BlockingRunner(std::shared_future<void> released) : released(std::move(released)) {}

  void run(std::string_view,
           std::string_view,
           std::string_view,
           std::shared_ptr<events::devices_atom_queue>,
           const immer::array<std::string> &,
           const immer::array<std::pair<std::string, std::string>> &,
           const immer::map<std::string, std::string> &,
           std::string_view,
           const std::function<bool()> &) override {
    released.wait();
  }

  events::RunnerTypes serialize() const override {
    return wolf::config::AppCMD{.run_cmd = ""};
  }
};
} // namespace

TEST_CASE("Runners get the PulseAudio socket mounted", "[PULSE]") {
  PrivatePulse pulse;
  // How libpulse names a socket picked from PULSE_SERVER: Docker would split a bind on that ':'
  auto audio_server = std::optional<sessions::AudioServer>(
      sessions::AudioServer{.server = keep_alive(audio::connect(fmt::format("unix:{}", pulse.socket)))});
  REQUIRE(audio::connected(audio_server->server));

  auto runner = std::make_shared<CaptureRunner>();
  std::string session_id = "1234";
  std::string folder = pulse.dir.string();
  sessions::start_runner(runner,
                         std::make_shared<events::devices_atom_queue>(),
                         immer::box<sessions::RunnerArgs>{sessions::RunnerArgs{
                             .session_id = session_id,
                             .video_settings = {},
                             .wayland_display = nullptr,
                             .audio_server = audio_server,
                             .audio_sink = nullptr,
                             .host = immer::box<state::Host>(state::Host{.host_base_state_folder = folder,
                                                                         .local_base_state_folder = folder,
                                                                         .host_xdg_runtime_dir = "/host/xdg"}),
                             .app_local_state_folder = folder,
                             .app_host_state_folder = folder,
                             .xdg_runtime_dir = folder,
                             .client_settings = immer::box<wolf::config::ClientSettings>()}});

  REQUIRE(runner->env.find("PULSE_SERVER"));
  bool mounted = false;
  for (const auto &[host_path, container_path] : runner->paths) {
    if (container_path == pulse.socket) {
      mounted = true;
      REQUIRE(host_path == "/host/xdg/native");
    }
    REQUIRE(container_path.find("unix:") == std::string::npos);
  }
  REQUIRE(mounted);
  audio::disconnect(audio_server->server);
}

TEST_CASE("A session's audio sink outlives its runners and goes with the session", "[PULSE]") {
  PrivatePulse pulse;
  auto audio_server = std::optional<sessions::AudioServer>(sessions::AudioServer{.server = connect(pulse)});

  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .host = immer::box<state::Host>(state::Host{}),
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});
  auto handlers = sessions::setup_moonlight_handlers(app_state, pulse.dir.string(), audio_server);

  // The session's own app keeps running until the test lets it go
  auto release_app = std::make_shared<std::promise<void>>();
  auto app_runner = std::make_shared<BlockingRunner>(release_app->get_future().share());
  // Also on a failed REQUIRE: the runner thread must not outlive the event bus
  auto release_on_exit = std::shared_ptr<void>(nullptr, [release_app](void *) {
    release_app->set_value();
    std::this_thread::sleep_for(200ms);
  });
  auto app = std::make_shared<events::App>(events::App{.base = {.title = "blocking", .id = "blocking"},
                                                       .start_virtual_compositor = false,
                                                       .start_audio_server = false, // the test creates the sink
                                                       .runner = app_runner});

  auto sink_name = fmt::format("{}4242", sessions::VIRTUAL_SINK_PREFIX);
  auto session = events::StreamSession{.event_bus = event_bus, .app = app, .session_id = 4242};
  session.audio_sink->store(create_sink(audio_server->server, sink_name));
  running_sessions->update([&](const auto &sessions) { return sessions.push_back(session); });
  event_bus->fire_event(immer::box<events::StreamSession>(session)); // starts the app runner
  settle(audio_server->server);
  REQUIRE(sink_exists(audio_server->server, sink_name));

  // Another runner of the session (API runners/start) exiting must leave the sink alone: the app still runs
  auto runner = std::make_shared<CaptureRunner>();
  event_bus->fire_event(immer::box<events::StartRunner>(
      events::StartRunner{.stop_stream_when_over = false,
                          .runner = runner,
                          .stream_session = std::make_shared<events::StreamSession>(session)}));
  for (int i = 0; i < 100 && !runner->ran; i++) {
    std::this_thread::sleep_for(20ms);
  }
  REQUIRE(runner->ran);
  std::this_thread::sleep_for(200ms); // let start_runner finish after run() returned
  settle(audio_server->server);
  REQUIRE(sink_exists(audio_server->server, sink_name));

  event_bus->fire_event(immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = 4242}));
  settle(audio_server->server);
  REQUIRE_FALSE(sink_exists(audio_server->server, sink_name));

  audio::disconnect(audio_server->server);
}

/**
 * A lobby's sink belongs to its runner once the runner starts (the runner's exit removes it), and to the lobby's stop
 * until then. Whoever claims `Lobby::runner_claimed` first owns it: the setup before starting the runner, the stop
 * handler before removing the sink. A stop right after the create isn't enough on its own to test the setup's claim:
 * a real runner's own is_stopped check would hide a missing one, so these runners don't check it.
 * Needs gst-wayland-display (waylanddisplaysrc) on GST_PLUGIN_PATH, like "Lobbies APIs".
 */
TEST_CASE("A lobby's audio sink goes with whoever claims its runner", "[PULSE]") {
  PrivatePulse pulse;
  auto audio_server = std::optional<sessions::AudioServer>(sessions::AudioServer{.server = connect(pulse)});

  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .host = immer::box<state::Host>(
          state::Host{.host_base_state_folder = pulse.dir.string(), .local_base_state_folder = pulse.dir.string()}),
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .lobbies = lobbies,
      .running_sessions = running_sessions});
  auto handlers = sessions::setup_lobbies_handlers(app_state, utils::get_env("XDG_RUNTIME_DIR", "/tmp"), audio_server);

  auto lobby_id = fmt::format("{}", 4300 + getpid() % 1000);
  auto sink_name = fmt::format("{}{}", sessions::VIRTUAL_SINK_PREFIX, lobby_id);
  std::atomic_int stops = 0;
  auto count_stops = event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
      [&](const immer::box<events::StopLobbyEvent> &ev) {
        if (ev->lobby_id == lobby_id) {
          stops++;
        }
      });
  auto create_lobby = [&](std::shared_ptr<events::Runner> runner) {
    auto create = events::CreateLobbyEvent{.id = lobby_id,
                                           .profile_id = "test_profile",
                                           .name = "test_lobby",
                                           .multi_user = false,
                                           .stop_when_everyone_leaves = false,
                                           .video_settings = {.width = 640,
                                                              .height = 480,
                                                              .refresh_rate = 30,
                                                              .wayland_render_node = "software",
                                                              .runner_render_node = "runner_render_node",
                                                              .video_producer_buffer_caps = "video/x-raw"},
                                           .audio_settings = {.channel_count = 2},
                                           .runner_state_folder = "runner_state_folder",
                                           .runner = runner};
    auto setup_over = create.on_setup_over.get()->get_future();
    event_bus->fire_event(immer::box<events::CreateLobbyEvent>(create));
    return setup_over;
  };
  auto stop_lobby = [&]() {
    event_bus->fire_event(immer::box<events::StopLobbyEvent>(events::StopLobbyEvent{.lobby_id = lobby_id}));
  };
  // The lobby's producers run on detached threads that log as they end: a test that returns first (this is the last
  // one in the binary) lets them log after main returned, into a destroyed logger (SIGSEGV/SIGABRT at exit)
  auto runtime_dir = std::filesystem::path(utils::get_env("XDG_RUNTIME_DIR", "/tmp"));
  auto wayland_sockets = [&]() {
    std::set<std::string> found;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(runtime_dir, ec)) {
      if (entry.path().filename().string().starts_with("wayland-")) {
        found.insert(entry.path().filename().string());
      }
    }
    return found;
  };
  auto sockets_before = wayland_sockets();
  auto wait_for_producers_to_end = [&]() {
    bool ended = false;
    for (int i = 0; i < 250 && !ended; i++) {
      ended = source_outputs(audio_server->server) == 0 && wayland_sockets() == sockets_before;
      if (!ended) {
        std::this_thread::sleep_for(20ms);
      }
    }
    CHECK(ended);
    std::this_thread::sleep_for(300ms); // their threads only unref the pipeline and log once stopped
  };

  SECTION("stopped during setup: the stop removes the sink and the runner never starts") {
    auto runner = std::make_shared<CaptureRunner>();
    create_lobby(runner);
    stop_lobby(); // the desktop takes far longer than this to come up
    // The setup lost the claim once it stops the lobby a second time (for producers that came up after the first)
    for (int i = 0; i < 1000 && stops < 2; i++) {
      std::this_thread::sleep_for(20ms);
    }
    REQUIRE(stops >= 2);
    std::this_thread::sleep_for(200ms); // a runner thread would have been started by now
    settle(audio_server->server);
    CHECK_FALSE(runner->ran);
    CHECK_FALSE(sink_exists(audio_server->server, sink_name));
    wait_for_producers_to_end();
  }

  SECTION("stopped while its runner runs: the sink stays until the runner exits") {
    auto release_app = std::make_shared<std::promise<void>>();
    auto runner = std::make_shared<BlockingRunner>(release_app->get_future().share());
    // Also on a failed REQUIRE: the runner thread must not outlive the event bus
    auto release_on_exit = std::shared_ptr<void>(nullptr, [release_app](void *) {
      release_app->set_value();
      std::this_thread::sleep_for(200ms);
    });
    REQUIRE(create_lobby(runner).wait_for(20s) == std::future_status::ready);
    settle(audio_server->server);
    REQUIRE(sink_exists(audio_server->server, sink_name));

    stop_lobby();
    std::this_thread::sleep_for(200ms); // the stop removes a sink on another thread
    settle(audio_server->server);
    CHECK(sink_exists(audio_server->server, sink_name)); // still the running app's

    release_on_exit.reset(); // the runner's exit removes it
    bool gone = false;
    for (int i = 0; i < 50 && !gone; i++) {
      settle(audio_server->server);
      gone = !sink_exists(audio_server->server, sink_name);
    }
    CHECK(gone);
    wait_for_producers_to_end();
  }

  audio::disconnect(audio_server->server);
}
