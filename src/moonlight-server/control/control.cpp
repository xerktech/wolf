#include "core/input.hpp"
#include <control/control.hpp>
#include <control/input_handler.hpp>
#include <events/events.hpp>
#include <immer/box.hpp>
#include <mutex>
#include <state/sessions.hpp>
#include <stdexcept>
#include <sys/socket.h>

namespace control {

using namespace ranges;
using namespace moonlight::control;
using namespace wolf::core::events;

void free_host(ENetHost *host) {
  std::for_each(host->peers, host->peers + host->peerCount, [](ENetPeer &peer_ref) {
    ENetPeer *peer = &peer_ref;

    if (peer) {
      enet_peer_disconnect_now(peer, 0);
    }
  });

  enet_host_destroy(host);
}

/**
 * ENet isn't thread-safe: enet_peer_send() appends to the lists that enet_host_service() drains. Feedback (rumble,
 * LEDs) and the StopStreamEvent's termination are sent from other threads, so every ENet call on a control host, except
 * the socket wait, holds this lock.
 */
static std::mutex enet_mutex;

/**
 * The longest run_control() waits for a client packet outside enet_mutex. ENet's own wait would also wake up for its
 * retransmit and ping timers, which we can't compute (enet_protocol_compute_wait_timeout is private).
 */
constexpr std::chrono::milliseconds max_unlocked_wait = 10ms;

using enet_host = std::unique_ptr<ENetHost, decltype(&free_host)>;
using enet_packet = std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)>;

bool init() {
  auto error_code = enet_initialize();
  if (error_code != 0) {
    logs::log(logs::error, "An error occurred while initializing Enet: {}.", error_code);
    return false;
  }
  return true;
}

enet_host create_host(std::string_view host, std::uint16_t port, std::size_t peers) {
  ENetAddress addr;
  enet_address_set_host(&addr, host.data());
  enet_address_set_port(&addr, port);

  auto enet_host = enet_host_create(AF_INET, &addr, peers, 0, 0, 0);
  if (enet_host == nullptr) {
    logs::log(logs::error, "An error occurred while trying to create an ENet server host.");
  }

  return {enet_host, free_host};
}

/**
 * The Moonlight fork of ENET doesn't include host and port as easily accessible parts of the struct,
 * we have to extract them manually
 */
std::pair<std::string /* ip */, int /* port */> get_ip(const sockaddr *const ip_addr) {
  char data[INET6_ADDRSTRLEN];

  auto family = ip_addr->sa_family;
  std::uint16_t port;
  if (family == AF_INET6) {
    inet_ntop(AF_INET6, &((sockaddr_in6 *)ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
    port = ((sockaddr_in6 *)ip_addr)->sin6_port;
  }

  if (family == AF_INET) {
    inet_ntop(AF_INET, &((sockaddr_in *)ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
    port = ((sockaddr_in *)ip_addr)->sin_port;
  }

  return {std::string{data}, port};
}

/**
 * Call with enet_mutex held. Flushes right away: run_control() may be waiting for a client packet.
 */
bool send_packet(std::string_view payload, ENetPeer *peer) {
  logs::log(logs::trace, "[ENET] Sending packet");
  auto packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
  if (enet_peer_send(peer, 0, packet) < 0) {
    logs::log(logs::warning, "[ENET] Failed to send packet");
    enet_packet_destroy(packet);
    return false;
  }
  enet_host_flush(peer->host);
  return true;
}

bool encrypt_and_send(std::string_view payload,
                      std::string_view aes_key,
                      const std::shared_ptr<events::ControlStreamState> &control_stream,
                      immer::box<std::shared_ptr<ENetPeer>> connected_client) {
  if (auto enet_client = connected_client->get()) {
    auto send = [&](control::ControlEncryption mode) {
      std::lock_guard lock(enet_mutex); // also keeps packets on the wire in send_seq order
      auto encrypted = control::encrypt_packet(aes_key, control_stream->send_seq++, payload, mode);
      return send_packet({(char *)encrypted.get(), encrypted->full_size()}, enet_client);
    };
    auto hint = *control_stream->encryption.load();
    if (control_stream->encryption_negotiated) {
      return send(hint);
    }
    // No client packet has authenticated yet, so the mode is only the (unauthenticated) ANNOUNCE's hint: a spoofed one
    // would make e.g. the termination unreadable. Send it in both modes; the client drops the copy it can't decrypt.
    auto other = hint == control::ControlEncryption::V1 ? control::ControlEncryption::V2
                                                        : control::ControlEncryption::V1;
    bool sent = send(hint);
    return send(other) || sent;
  } else {
    logs::log(logs::warning, "[ENET] Failed to send packet, client is not connected");
    return false;
  }
}

bool has_connected_peer(const enet_clients_map &connected_clients, const events::StreamSession &session) {
  for (const auto &[peer, peer_session] : connected_clients) {
    if (peer_session->session_id == session.session_id &&
        peer_session->enet_secret_payload == session.enet_secret_payload) {
      return true;
    }
  }
  return false;
}

std::optional<events::StreamSession> get_current_session(const enet_clients_map &connected_clients,
                                                         const state::SessionsAtoms &running_sessions,
                                                         std::string_view client_ip,
                                                         const ENetEvent &enet_event) {
  if (enet_event.type == ENET_EVENT_TYPE_CONNECT) {
    auto sessions = running_sessions->load(); // iterate a snapshot that an update can't free under us
    // A new connection, we should check if there's a session that matches the current client
    for (const StreamSession &session : *sessions) {
      if (session.enet_secret_payload == enet_event.data) {
        return session;
      }
    }
    logs::log(logs::warning,
              "[ENET] Unable to find a session that matches the client secret {}, matching by IP",
              enet_event.data);
    // Never for a stream instance that already has a connected peer: anyone behind the same IP (NAT) could otherwise
    // attach to a live stream and, being the newest peer, drop its real client (see superseded_peers).
    // Peers of an instance a /resume replaced don't count: they're the client's dead connection, waiting to time out.
    for (const StreamSession &session : *sessions) {
      if (session.ip == client_ip && !has_connected_peer(connected_clients, session)) {
        return session;
      }
    }
  } else {
    // The connection has already been established, we'll check for a match in our connected client map
    if (auto client = connected_clients.find(enet_event.peer)) {
      return client->get();
    }
  }
  return std::nullopt;
}

std::shared_ptr<ENetPeer> to_shared_ptr(ENetPeer *peer) {
  return std::shared_ptr<ENetPeer>(peer, [](auto _peer) {
    // DO NOTHING, we don't want to free peer, the lifecycle is dictated by enet
  });
}

immer::vector<ENetPeer *>
superseded_peers(const enet_clients_map &connected_clients, const ENetPeer *new_peer, std::size_t session_id) {
  immer::vector<ENetPeer *> stale;
  for (const auto &[peer, session] : connected_clients) {
    if (peer != new_peer && session->session_id == session_id) {
      stale = stale.push_back(peer);
    }
  }
  return stale;
}

bool is_superseded_stream(const immer::vector<events::StreamSession> &running_sessions,
                          const events::StreamSession &peer_session) {
  for (const auto &session : running_sessions) {
    if (session.session_id == peer_session.session_id) {
      return session.enet_secret_payload != peer_session.enet_secret_payload;
    }
  }
  return false;
}

void end_superseded_stream(const events::StreamSession &peer_session,
                           const std::shared_ptr<events::EventBusType> &event_bus) {
  event_bus->fire_event(immer::box<EndSupersededStreamEvent>(
      EndSupersededStreamEvent{.session_id = peer_session.session_id,
                               .rtp_secret_payload = peer_session.rtp_secret_payload}));
}

std::optional<std::pair<ENetPeer *, immer::box<events::StreamSession>>>
find_connected_peer(const immer::atom<enet_clients_map> &connected_clients, std::size_t session_id) {
  auto clients = connected_clients.load(); // iterate a snapshot that an update can't free under us
  for (auto &[peer, session] : *clients) {
    if (session->session_id == session_id) {
      return {{peer, session}};
    }
  }
  return std::nullopt;
}

/**
 * Pause the stream that `peer_session` attached to. If a /resume has already replaced it with a newer one, only end
 * the old stream instance's pipelines: a PauseStreamEvent is keyed by session_id and would end the new stream too.
 */
void pause_stream(const state::SessionsAtoms &running_sessions,
                  const events::StreamSession &peer_session,
                  const std::shared_ptr<events::EventBusType> &event_bus) {
  // One snapshot for both checks, so a /resume landing between them can't turn a superseded stream into a pause
  auto running = running_sessions->load();
  if (is_superseded_stream(*running, peer_session)) {
    logs::log(logs::debug, "[ENET] Ending superseded stream of session: {}", peer_session.session_id);
    end_superseded_stream(peer_session, event_bus);
    return;
  }
  if (!state::get_session_by_id(*running, peer_session.session_id)) {
    // Already stopped. A /resume may be about to add a new stream under the same session_id: a pause would end it.
    logs::log(logs::debug, "[ENET] Not pausing stopped stream of session: {}", peer_session.session_id);
    return;
  }
  event_bus->fire_event(immer::box<PauseStreamEvent>(PauseStreamEvent{.session_id = peer_session.session_id}));
}

void run_control(int port,
                 const state::SessionsAtoms &running_sessions,
                 const std::shared_ptr<events::EventBusType> &event_bus,
                 int peers,
                 std::chrono::milliseconds timeout,
                 const std::string &host_ip) {

  enet_host host = create_host(host_ip, port, peers);
  if (!host) { // e.g. the port is already taken; servicing a null host would segfault
    throw std::runtime_error(fmt::format("Unable to bind the ENet control server on port {}", port));
  }
  logs::log(logs::info, "Control server started on port: {}", port);

  ENetEvent event;

  immer::atom<enet_clients_map> connected_clients;

  auto stop_ev = event_bus->register_handler<immer::box<StopStreamEvent>>(
      [&connected_clients](const immer::box<StopStreamEvent> &ev) {
        auto terminate_pkt = ControlTerminatePacket{};
        std::string plaintext = {(char *)&terminate_pkt, sizeof(terminate_pkt)};
        if (auto connected = find_connected_peer(connected_clients, ev->session_id)) {
          auto &[peer, session] = *connected;
          immer::box<std::shared_ptr<ENetPeer>> enet_client = {to_shared_ptr(peer)};
          encrypt_and_send(plaintext, session->aes_key, session->control_stream, enet_client);
          return;
        }
        logs::log(logs::debug, "[ENET] Client not found for session: {}", ev->session_id);
      });

  while (true) {
    int serviced;
    {
      std::lock_guard lock(enet_mutex);
      serviced = enet_host_service(host.get(), &event, 0);
    }
    if (serviced <= 0) {
      // Wait outside the lock, so that other threads can send meanwhile
      enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
      enet_socket_wait(host->socket, &condition, std::min(timeout, max_unlocked_wait).count());
    } else {
      auto [client_ip, client_port] = get_ip((sockaddr *)&event.peer->address.address);
      auto client_session = get_current_session(connected_clients, running_sessions, client_ip, event);
      if (client_session) {
        switch (event.type) {
        case ENET_EVENT_TYPE_NONE:
          break;
        case ENET_EVENT_TYPE_CONNECT:
          logs::log(logs::debug, "[ENET] connected client: {}:{}", client_ip, client_port);
          {
            // The newest peer owns the session: forget older peers of the same session (a dead client after a
            // /resume) so that their late disconnect can't pause the stream this peer is now attached to.
            auto connected = connected_clients.load();
            auto stale = superseded_peers(*connected, event.peer, client_session->session_id);
            connected_clients.update([peer = event.peer, client_session, stale](const enet_clients_map &m) {
              auto updated = m;
              for (auto stale_peer : stale) {
                updated = updated.erase(stale_peer);
              }
              return updated.set(peer, client_session.value());
            });
            for (auto stale_peer : stale) {
              logs::log(logs::debug, "[ENET] Dropping superseded peer for session: {}", client_session->session_id);
              {
                std::lock_guard lock(enet_mutex);
                enet_peer_disconnect_now(stale_peer, 0);
              }
              // Same stream instance reconnecting: its pipelines are the ones this peer is attached to, keep them
              auto stale_session = connected->at(stale_peer);
              if (stale_session->enet_secret_payload != client_session->enet_secret_payload) {
                end_superseded_stream(stale_session, event_bus);
              }
            }
            // Joypads (carried over by a /resume) send their feedback to this peer from now on
            client_session->control_feedback->store(events::ControlFeedback{
                .peer = event.peer,
                .send = [peer = to_shared_ptr(event.peer),
                         aes_key = client_session->aes_key,
                         control_stream = client_session->control_stream](std::string_view plaintext) {
                  encrypt_and_send(plaintext, aes_key, control_stream, immer::box<std::shared_ptr<ENetPeer>>{peer});
                }});
            // Joypads kept from its last stream that the client doesn't claim by then: it brought fewer this time
            release_unclaimed_joypads_after(running_sessions, *client_session, unclaimed_joypads_timeout);
          }
          event_bus->fire_event(
              immer::box<ResumeStreamEvent>(ResumeStreamEvent{.session_id = client_session->session_id}));
          break;
        case ENET_EVENT_TYPE_DISCONNECT:
          logs::log(logs::debug, "[ENET] disconnected client: {}:{}", client_ip, client_port);
          connected_clients.update([peer = event.peer](const enet_clients_map &m) { return m.erase(peer); });
          // ENet reuses the peer's slot for the next connection: never send feedback to it again
          client_session->control_feedback->update([peer = event.peer](const events::ControlFeedback &feedback) {
            return feedback.peer == peer ? events::ControlFeedback{} : feedback;
          });
          pause_stream(running_sessions, client_session.value(), event_bus);
          break;
        case ENET_EVENT_TYPE_RECEIVE:
          enet_packet packet = {event.packet, enet_packet_destroy};

          if (packet->dataLength < sizeof(ControlPacket)) {
            logs::log(logs::warning, "[ENET] Dropping {} byte packet, too short for a header", packet->dataLength);
            break;
          }

          auto type = ((ControlPacket *)packet->data)->type;

          logs::log(logs::trace,
                    "[ENET] received {} of {} bytes from: {}:{} HEX: {}",
                    packet_type_to_str(type),
                    packet->dataLength,
                    client_ip,
                    client_port,
                    crypto::str_to_hex({(char *)packet->data, packet->dataLength}));

          if (type == ENCRYPTED) {
            try {
              auto enc_pkt = (ControlEncryptedPacket *)(packet->data);
              if (!enc_pkt->fits_in(packet->dataLength)) {
                logs::log(logs::warning,
                          "[ENET] Dropping {} byte encrypted packet, its header claims {} bytes",
                          packet->dataLength,
                          enc_pkt->full_size());
                break;
              }
              auto control_stream = client_session->control_stream;
              if (!control_stream->encryption_negotiated) {
                // The first packet that authenticates fixes the mode: what the (unauthenticated) RTSP ANNOUNCE
                // asked for is only a hint, a spoofed one sent before or after the client's own mustn't decide it
                auto mode = authenticated_mode(*enc_pkt, client_session->aes_key);
                if (!mode) {
                  logs::log(logs::warning, "[ENET] Dropping encrypted packet that doesn't authenticate");
                  break;
                }
                if (*mode != *control_stream->encryption.load()) {
                  logs::log(logs::info,
                            "[ENET] Control stream encryption differs from the ANNOUNCE, using the client's");
                }
                control_stream->authenticate_encryption(*mode);
              }
              auto encryption = *control_stream->encryption.load();
              auto decrypted = decrypt_packet(*enc_pkt, client_session->aes_key, encryption);
              if (decrypted.size() < sizeof(ControlPacket)) {
                logs::log(logs::warning, "[ENET] Dropping decrypted packet of {} bytes", decrypted.size());
                break;
              }

              // V1 doesn't authenticate seq past its low byte, so there's no seq to check for a replay.
              // With V2 the tag covers the whole seq. Only this thread updates the window.
              if (encryption == ControlEncryption::V2) {
                auto seq = boost::endian::little_to_native(enc_pkt->seq);
                auto window = accept_seq(*control_stream->replay_window.load(), seq);
                if (!window) {
                  logs::log(logs::warning, "[ENET] Dropping encrypted packet with seq {}, replayed or too old", seq);
                  break;
                }
                control_stream->replay_window.store(*window);
              }
              auto sub_type = ((ControlPacket *)decrypted.data())->type;

              logs::log(logs::trace,
                        "[ENET] decrypted sub_type: {} HEX: {}",
                        packet_type_to_str(sub_type),
                        crypto::str_to_hex(decrypted));

              if (sub_type == TERMINATION) {
                pause_stream(running_sessions, client_session.value(), event_bus);
              } else if (sub_type == INPUT_DATA) {
                immer::box<std::shared_ptr<ENetPeer>> enet_client = {to_shared_ptr(event.peer)};
                handle_input(client_session.value(), enet_client, (INPUT_PKT *)decrypted.data(), decrypted.size());
              } else if (sub_type == IDR_FRAME) {
                auto ev = IDRRequestEvent{.session_id = client_session->session_id};
                event_bus->fire_event(immer::box<IDRRequestEvent>{ev});
              }
            } catch (std::runtime_error &e) {
              logs::log(logs::warning, "[ENET] Unable to decrypt incoming packet: {}", e.what());
            }
          } else {
            logs::log(logs::warning,
                      "[ENET] Received unencrypted message: {} - {}",
                      packet_type_to_str(type),
                      crypto::str_to_hex({(char *)packet->data, packet->dataLength}));
          }
          break;
        }
      } else {
        logs::log(logs::warning, "[ENET] Received packet from unrecognised client {}:{}", client_ip, client_port);
        std::lock_guard lock(enet_mutex);
        enet_peer_disconnect_now(event.peer, 0);
      }
    }
  }

  stop_ev.unregister();
}

} // namespace control