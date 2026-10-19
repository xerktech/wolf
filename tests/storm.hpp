#pragma once

/**
 * Helpers for race-storm tests: code reading an immer::atom races a thread that keeps replacing the atom's value.
 *
 * A read of a snapshot freed by a concurrent update almost never crashes a normal build, so these tests only fail
 * reliably under AddressSanitizer (the CI ASan job), where the first read of freed memory aborts the run.
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <thread>
#include <vector>

/**
 * Silences logging below fatal for its scope: a storm logs a line or two per iteration, which would flood the console
 * that CI streams to the runner.
 */
struct QuietLogs {
  QuietLogs() {
    boost::log::core::get()->set_filter(boost::log::trivial::severity >= logs::fatal);
  }

  ~QuietLogs() {
    // Same level as tests/main.cpp
    auto level = logs::parse_level(utils::get_env("WOLF_LOG_LEVEL", "TRACE"));
    boost::log::core::get()->set_filter(boost::log::trivial::severity >= level);
  }
};

/**
 * Calls `mutate` in a loop on its own thread until destroyed.
 */
class Hammer {
public:
  explicit Hammer(std::function<void()> mutate)
      : thread([this, mutate = std::move(mutate)]() {
          while (!stopped) {
            mutate();
            iterations++;
          }
        }) {}

  ~Hammer() {
    stop();
  }

  /** Stops the loop; returns how many times `mutate` ran */
  std::size_t stop() {
    stopped = true;
    if (thread.joinable()) {
      thread.join();
    }
    return iterations;
  }

  std::atomic_size_t iterations = 0;

private:
  std::atomic_bool stopped = false;
  std::thread thread;
};

struct StormResult {
  std::size_t runs = 0;
  std::size_t failures = 0;
};

/**
 * Runs `reader` on `threads` threads, each until it has run `per_thread` times or `budget` has elapsed.
 * `reader` returns false on a wrong result: Catch2's assertions aren't thread safe, so check them on the test thread.
 */
inline StormResult
storm(int threads, std::size_t per_thread, std::chrono::milliseconds budget, const std::function<bool()> &reader) {
  auto deadline = std::chrono::steady_clock::now() + budget;
  std::atomic_size_t runs = 0, failures = 0;
  std::vector<std::thread> readers;
  for (int t = 0; t < threads; t++) {
    readers.emplace_back([&]() {
      for (std::size_t i = 0; i < per_thread && std::chrono::steady_clock::now() < deadline; i++) {
        if (!reader()) {
          failures++;
        }
        runs++;
      }
    });
  }
  for (auto &reader_thread : readers) {
    reader_thread.join();
  }
  return {.runs = runs, .failures = failures};
}
