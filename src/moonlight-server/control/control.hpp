#pragma once

#include <chrono>
#include <enet/enet.h>
#include <events/events.hpp>
#include <helpers/logger.hpp>
#include <moonlight/control.hpp>
#include <mutex>
#include <range/v3/view.hpp>
#include <state/data-structures.hpp>
#include <thread>

namespace control {

using namespace std::chrono_literals;
using namespace wolf::core;

void run_control(int port,
                 const state::SessionsAtoms &running_sessions,
                 const std::shared_ptr<events::EventBusType> &event_bus,
                 int peers = 20,
                 std::chrono::milliseconds timeout = 1000ms,
                 const std::string &host_ip = "0.0.0.0");

using enet_clients_map = immer::map<ENetPeer *, immer::box<events::StreamSession>>;

/**
 * A connected Moonlight client, as seen from threads other than the control thread.
 *
 * ENet is not thread-safe: every call on a host or on one of its peers must hold `host_mutex`.
 * Rumble/LED/adaptive-trigger callbacks run on inputtino's threads and StopStreamEvent on the event-bus thread,
 * while the control thread services the same host, so they must never touch `peer` without that lock.
 */
struct EnetClient {
  ENetPeer *peer;
  std::shared_ptr<std::mutex> host_mutex;
};

bool encrypt_and_send(std::string_view payload,
                      std::string_view aes_key,
                      immer::box<std::shared_ptr<EnetClient>> connected_client);

bool init();

} // namespace control