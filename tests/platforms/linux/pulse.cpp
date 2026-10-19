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
#include <pulse/pulseaudio.h>
#include <sessions/common.hpp>
#include <sessions/handlers.hpp>
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

std::shared_ptr<audio::Server> connect(const PrivatePulse &pulse) {
  auto server = audio::connect(pulse.socket);
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

  SECTION("leftover sinks are removed by prefix") {
    auto stale_name = fmt::format("{}1234", sessions::VIRTUAL_SINK_PREFIX);
    auto stale = create_sink(server, stale_name);
    auto other = create_sink(server, "wolf_test_not_ours");
    settle(server);

    auto found = audio::find_virtual_sinks(server, sessions::VIRTUAL_SINK_PREFIX, 2s);
    REQUIRE(found.size() == 1);

    // Sessions started after the snapshot: one new, one of the same client as the leftover (session ids are stable
    // across restarts), whose sink PulseAudio names <name>.2 since the leftover still holds the name
    auto live = create_sink(server, fmt::format("{}5678", sessions::VIRTUAL_SINK_PREFIX));
    auto same_client = create_sink(server, stale_name);
    settle(server);

    audio::unload_virtual_sinks(server, found);
    settle(server);
    REQUIRE_FALSE(sink_exists(server, stale_name));
    REQUIRE(sink_exists(server, fmt::format("{}.2", stale_name)));
    REQUIRE(sink_exists(server, fmt::format("{}5678", sessions::VIRTUAL_SINK_PREFIX)));
    REQUIRE(sink_exists(server, "wolf_test_not_ours"));
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
      sessions::AudioServer{.server = audio::connect(fmt::format("unix:{}", pulse.socket))});
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
