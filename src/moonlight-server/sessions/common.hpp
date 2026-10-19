#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>

#include <helpers/logger.hpp>
#include <helpers/utils.hpp>

namespace wolf::core::sessions {

constexpr std::string_view VIRTUAL_SINK_PREFIX = "virtual_sink_";
constexpr std::chrono::milliseconds DEFAULT_WAYLAND_SOCKET_WAIT_TIMEOUT = std::chrono::seconds(5);

/**
 * The socket path of a PulseAudio server name as libpulse writes it ("/run/pulse/native", "unix:/run/pulse/native",
 * optionally behind a "{machine-id}" prefix), or "" when it isn't a local socket (e.g. "tcp:host:4713").
 */
inline std::string audio_server_socket_path(std::string_view server_name) {
  if (server_name.starts_with('{')) {
    auto end = server_name.find('}');
    server_name.remove_prefix(end == std::string_view::npos ? server_name.size() : end + 1);
  }
  constexpr std::string_view unix_prefix = "unix:";
  if (server_name.starts_with(unix_prefix)) {
    server_name.remove_prefix(unix_prefix.size());
  }
  return server_name.starts_with('/') ? std::string(server_name) : std::string();
}

inline std::chrono::milliseconds get_wayland_socket_wait_timeout() {
  auto timeout_ms = utils::get_env("WOLF_WAYLAND_SOCKET_WAIT_TIMEOUT_MS");
  if (timeout_ms) {
    try {
      auto parsed = std::stoll(timeout_ms);
      if (parsed >= 0) {
        return std::chrono::milliseconds(parsed);
      }
      logs::log(logs::warning,
                "Ignoring negative WOLF_WAYLAND_SOCKET_WAIT_TIMEOUT_MS={}, using default {}ms",
                timeout_ms,
                DEFAULT_WAYLAND_SOCKET_WAIT_TIMEOUT.count());
    } catch (const std::exception &) {
      logs::log(logs::warning,
                "Ignoring invalid WOLF_WAYLAND_SOCKET_WAIT_TIMEOUT_MS={}, using default {}ms",
                timeout_ms,
                DEFAULT_WAYLAND_SOCKET_WAIT_TIMEOUT.count());
    }
  }

  return DEFAULT_WAYLAND_SOCKET_WAIT_TIMEOUT;
}

/**
 * Waits until the compositor's Wayland socket exists in runtime_dir; false on timeout.
 * `has_compositor` is false for apps with start_virtual_compositor = false: they get no socket at all.
 */
inline bool wait_for_wayland_socket(std::string_view runtime_dir,
                                    const std::string &socket_name,
                                    bool has_compositor,
                                    std::chrono::milliseconds timeout = get_wayland_socket_wait_timeout()) {
  if (!has_compositor) {
    return true;
  }
  if (socket_name.empty()) { // the runner would start with no WAYLAND_DISPLAY and fail to find the desktop
    logs::log(logs::error, "The virtual compositor didn't report a Wayland socket");
    return false;
  }

  if (auto skip_wait = utils::get_env("WOLF_SKIP_WAYLAND_SOCKET_WAIT")) {
    if (std::string_view(skip_wait) == "TRUE" || std::string_view(skip_wait) == "1") {
      return true;
    }
  }

  auto socket_path = std::filesystem::path(runtime_dir) / socket_name;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  struct stat st {};

  // Check at least once, so a timeout of 0 means "don't wait" rather than "always fail"
  while (true) {
    if (stat(socket_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (stat(socket_path.c_str(), &st) == 0) {
    logs::log(logs::error,
              "Wayland endpoint {} exists but is not a socket (mode={:o})",
              socket_path.string(),
              st.st_mode);
  } else {
    logs::log(logs::error, "Wayland socket {} was never created", socket_path.string());
  }

  return false;
}
} // namespace wolf::core::sessions
