#define CATCH_CONFIG_FAST_COMPILE

#include <catch2/catch_session.hpp>
#include <control/control.hpp>
#include <core/docker.hpp>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <streaming/streaming.hpp>
#include <unistd.h>

int main(int argc, char *argv[]) {
  logs::init(logs::parse_level(utils::get_env("WOLF_LOG_LEVEL", "TRACE")));
  streaming::init();
  control::init();       // Need to initialise enet once
  state::docker::init(); // Need to initialise libcurl once

  int result = Catch::Session().run(argc, argv);

  // Leave without running static destructors, like wolf itself (see wolf.cpp): the API servers and SSE clients the
  // tests detach are still running, and would touch destroyed statics (the logger, the SSE keepalive) during exit().
  // _exit, not _Exit: TSan intercepts _exit, so a reported race still turns the exit code into 66.
  std::clog.flush();
  std::cout.flush();
  std::fflush(nullptr);
  ::_exit(result);
}
