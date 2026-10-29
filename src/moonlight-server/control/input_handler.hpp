#pragma once

#include <chrono>
#include <control/control.hpp>
#include <events/events.hpp>
#include <moonlight/control.hpp>

namespace control {

using namespace moonlight::control::pkts;
using namespace wolf::core;

/**
 * Dispatches a raw input packet of pkt_len bytes; packets shorter than their type requires are dropped.
 * Side effect: session devices might be updated when hotplugging
 */
void handle_input(events::StreamSession &session,
                  immer::box<std::shared_ptr<ENetPeer>> connected_client,
                  const INPUT_PKT *pkt,
                  std::size_t pkt_len);

void mouse_move_rel(const MOUSE_MOVE_REL_PACKET &pkt, events::StreamSession &session);

void mouse_move_abs(const MOUSE_MOVE_ABS_PACKET &pkt, events::StreamSession &session);

void mouse_button(const MOUSE_BUTTON_PACKET &pkt, events::StreamSession &session);

void mouse_scroll(const MOUSE_SCROLL_PACKET &pkt, events::StreamSession &session);

void mouse_h_scroll(const MOUSE_HSCROLL_PACKET &pkt, events::StreamSession &session);

void keyboard_key(const KEYBOARD_PACKET &pkt, events::StreamSession &session);

void utf8_text(const UTF8_TEXT_PACKET &pkt, events::StreamSession &session);

void touch(const TOUCH_PACKET &pkt, events::StreamSession &session);

void pen(const PEN_PACKET &pkt, events::StreamSession &session);

void controller_arrival(const CONTROLLER_ARRIVAL_PACKET &pkt,
                        events::StreamSession &session,
                        immer::box<std::shared_ptr<ENetPeer>> connected_client);

void controller_multi(const CONTROLLER_MULTI_PACKET &pkt,
                      events::StreamSession &session,
                      immer::box<std::shared_ptr<ENetPeer>> connected_client);

/**
 * How long after its client connects a stream waits for the client to claim the joypads it took over from a lobby
 * (StreamSession::unclaimed_joypads) before release_unclaimed_joypads.
 */
constexpr std::chrono::seconds unclaimed_joypads_timeout{10};

/**
 * Unplugs the joypads the stream took over from a lobby that its client hasn't claimed (with a CONTROLLER_ARRIVAL or
 * the pad's input): the client brought fewer or other controllers this time.
 */
void release_unclaimed_joypads(events::StreamSession &session);

void controller_touch(const CONTROLLER_TOUCH_PACKET &pkt, events::StreamSession &session);

void controller_motion(const CONTROLLER_MOTION_PACKET &pkt, events::StreamSession &session);

void controller_battery(const CONTROLLER_BATTERY_PACKET &pkt, events::StreamSession &session);

} // namespace control
