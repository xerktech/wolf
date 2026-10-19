#include <core/audio.hpp>
#include <helpers/logger.hpp>

namespace wolf::core::audio {

struct Server {};

std::shared_ptr<Server> connect(std::string_view server) {
  logs::log(logs::warning, "Virtual audio unsupported for the current platform.");
  return std::make_shared<Server>();
}

std::shared_ptr<VSink> create_virtual_sink(const std::shared_ptr<Server> &server, const AudioDevice &device) {
  return std::make_shared<VSink>();
}

void delete_virtual_sink(const std::shared_ptr<Server> &server, const std::shared_ptr<VSink> &vsink) {}

void remove_stale_virtual_sinks(const std::shared_ptr<Server> &server,
                                std::string_view sink_name_prefix,
                                const std::function<bool(std::string_view sink_name)> &in_use) {}

bool connected(const std::shared_ptr<Server> &server) {
  return false;
}

bool flush(const std::shared_ptr<Server> &server, std::chrono::milliseconds timeout) {
  return true;
}

void disconnect(const std::shared_ptr<Server> &server) {}

std::string get_server_name(const std::shared_ptr<Server> &server) {
  return "";
}

} // namespace wolf::core::audio