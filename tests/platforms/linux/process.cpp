#include <boost/asio/ip/tcp.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <runners/process.hpp>
#include <sstream>
#include <sys/stat.h>
#include <thread>

using Catch::Matchers::ContainsSubstring;

TEST_CASE("Process runner doesn't leak Wolf's sockets to the child", "[PROCESS]") {
  // Stand-in for Wolf's HTTP/RTSP listeners, which are not opened with close-on-exec
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
  struct stat sock_stat {};
  REQUIRE(fstat(acceptor.native_handle(), &sock_stat) == 0);
  auto listen_socket = fmt::format("socket:[{}]", sock_stat.st_ino);

  auto out_file = std::filesystem::temp_directory_path() / fmt::format("wolf-process-fds-{}", getpid());
  std::filesystem::remove(out_file);

  auto runner = process::RunProcess(std::make_shared<wolf::core::events::EventBusType>(),
                                    fmt::format("/bin/sh -c \"ls -l /proc/$$/fd > {}\"", out_file.string()));
  runner.run("1", "", "", std::make_shared<wolf::core::events::devices_atom_queue>(), {}, {}, {}, "");

  std::ifstream in(out_file);
  std::stringstream child_fds;
  child_fds << in.rdbuf();
  std::filesystem::remove(out_file);

  REQUIRE_THAT(child_fds.str(), ContainsSubstring(" 2 -> ")); // the child ran, and kept its stderr
  REQUIRE_THAT(child_fds.str(), !ContainsSubstring(listen_socket));
}

TEST_CASE("Process runner survives a repeated stop", "[PROCESS]") {
  using namespace std::chrono_literals;
  auto ev_bus = std::make_shared<wolf::core::events::EventBusType>();
  auto runner = process::RunProcess(ev_bus, "sleep 10");
  // Stand in for every other StopStreamEvent handler (pipelines, session removal). The bus doesn't promise an order,
  // so register one before and one after the runner's: one of them runs after it.
  std::atomic_int before_calls = 0, after_calls = 0;
  auto before_handler = ev_bus->register_handler<immer::box<wolf::core::events::StopStreamEvent>>(
      [&before_calls](const immer::box<wolf::core::events::StopStreamEvent> &) { before_calls++; });

  auto start = std::chrono::steady_clock::now();
  std::thread run_thread([&runner]() {
    runner.run("42", "", "", std::make_shared<wolf::core::events::devices_atom_queue>(), {}, {}, {}, "");
  });
  std::this_thread::sleep_for(500ms); // let the runner spawn the app and register its stop handlers
  auto after_handler = ev_bus->register_handler<immer::box<wolf::core::events::StopStreamEvent>>(
      [&after_calls](const immer::box<wolf::core::events::StopStreamEvent> &) { after_calls++; });

  // The first stop kills the process group; the second reaches the handler before the runner has seen the exit and
  // unregistered it. Terminating an already terminated group throws (killpg EINVAL), and the event bus swallows a
  // std::system_error by skipping every handler after the throwing one.
  auto stop = immer::box<wolf::core::events::StopStreamEvent>(wolf::core::events::StopStreamEvent{.session_id = 42});
  ev_bus->fire_event(stop);
  ev_bus->fire_event(stop);

  run_thread.join();
  REQUIRE(before_calls == 2);
  REQUIRE(after_calls == 2);
  REQUIRE(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("Process runner returns when its app exits while sibling runners still run", "[PROCESS]") {
  using namespace std::chrono_literals;
  auto ev_bus = std::make_shared<wolf::core::events::EventBusType>();
  auto short_runner = process::RunProcess(ev_bus, "/bin/true");
  auto long_runner = process::RunProcess(ev_bus, "sleep 30");

  // A sibling app spawned while a runner's stdout/stderr pipes are still open in Wolf (between creating them and
  // spawning its own app) used to inherit their write ends: that runner then waited for the sibling to exit as well,
  // and its app stayed an unreaped zombie meanwhile. Spawn many at once so the windows overlap.
  constexpr int pairs = 16;
  std::atomic_int slow_short_runners = 0;
  std::vector<std::thread> threads;
  for (int i = 0; i < pairs; i++) {
    threads.emplace_back([&short_runner, &slow_short_runners]() {
      auto start = std::chrono::steady_clock::now();
      short_runner.run("1", "", "", std::make_shared<wolf::core::events::devices_atom_queue>(), {}, {}, {}, "");
      if (std::chrono::steady_clock::now() - start > 4s) {
        slow_short_runners++;
      }
    });
    threads.emplace_back([&long_runner]() {
      long_runner.run("2", "", "", std::make_shared<wolf::core::events::devices_atom_queue>(), {}, {}, {}, "");
    });
  }
  // The short runners are done long before the long apps would exit on their own
  std::this_thread::sleep_for(5s);
  ev_bus->fire_event(
      immer::box<wolf::core::events::StopStreamEvent>(wolf::core::events::StopStreamEvent{.session_id = 2}));
  for (auto &t : threads) {
    t.join();
  }
  REQUIRE(slow_short_runners == 0);
}
