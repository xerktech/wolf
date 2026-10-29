#include "libinput.h"
#include <boost/endian/conversion.hpp>
#include <boost/locale.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <chrono>
#include <control/input_handler.hpp>
#include <cstring>
#include <platforms/input.hpp>
#include <platforms/linux/uinput/uinput.hpp>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Equals;
using Catch::Matchers::StartsWith;
using Catch::Matchers::WithinRel;

using namespace wolf::core::input;
using namespace wolf::core;
using namespace moonlight::control;
using namespace std::string_literals;

TEST_CASE("uinput - keyboard", "[UINPUT]") {
  libevdev_ptr keyboard_dev(libevdev_new(), ::libevdev_free);
  auto session = events::StreamSession{
      .keyboard = std::make_shared<std::optional<events::KeyboardTypes>>(std::move(*Keyboard::create()))};
  link_devnode(keyboard_dev.get(), std::get<state::input::Keyboard>(session.keyboard->value()).get_nodes()[0]);

  auto events = fetch_events_debug(keyboard_dev);
  REQUIRE(events.empty());

  auto press_shift_key = pkts::KEYBOARD_PACKET{.key_code = boost::endian::native_to_little((short)0xA0)};
  press_shift_key.type = pkts::KEY_PRESS;

  control::handle_input(session, {}, &press_shift_key, sizeof(press_shift_key));
  events = fetch_events_debug(keyboard_dev);
  REQUIRE(events.size() == 1);
  REQUIRE_THAT(libevdev_event_type_get_name(events[0]->type), Equals("EV_KEY"));
  REQUIRE_THAT(libevdev_event_code_get_name(events[0]->type, events[0]->code), Equals("KEY_LEFTSHIFT"));
  REQUIRE(events[0]->value == 1);

  auto release_shift_key = pkts::KEYBOARD_PACKET{.key_code = boost::endian::native_to_little((short)0xA0)};
  release_shift_key.type = pkts::KEY_RELEASE;

  control::handle_input(session, {}, &release_shift_key, sizeof(release_shift_key));
  events = fetch_events_debug(keyboard_dev);
  REQUIRE(events.size() == 1);
  REQUIRE_THAT(libevdev_event_type_get_name(events[0]->type), Equals("EV_KEY"));
  REQUIRE_THAT(libevdev_event_code_get_name(events[0]->type, events[0]->code), Equals("KEY_LEFTSHIFT"));
  REQUIRE(events[0]->value == 0);
}

TEST_CASE("uinput - pen tablet", "[UINPUT]") {
  auto session = events::StreamSession{.event_bus = std::make_shared<events::EventBusType>()};
  auto packet = pkts::PEN_PACKET{.event_type = pkts::TOUCH_EVENT_HOVER,
                                 .tool_type = pkts::TOOL_TYPE_PEN,
                                 .pen_buttons = pkts::PEN_BUTTON_TYPE_PRIMARY,
                                 .x = {0, 0, 0, 63},                    // 0.5 in float (little endian)
                                 .y = {0, 0, 0, 63},                    // 0.5 in float (little endian)
                                 .pressure_or_distance = {0, 0, 0, 63}, // 0.5 in float (little endian)
                                 .rotation = 0,
                                 .tilt = 0};
  packet.type = pkts::PEN;

  control::handle_input(session, {}, &packet, sizeof(packet));

  REQUIRE(session.pen_tablet->has_value()); // Should create a pen when a packet arrives
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  auto li = create_libinput_context(session.pen_tablet->value().get_nodes());
  auto event = get_event(li);
  REQUIRE(event);
  REQUIRE(libinput_event_get_type(event.get()) == LIBINPUT_EVENT_DEVICE_ADDED);
  REQUIRE(libinput_device_has_capability(libinput_event_get_device(event.get()), LIBINPUT_DEVICE_CAP_TABLET_TOOL));

  // Changing something just so that libinput picks up the event
  packet.rotation = 90;
  packet.tilt = 45;
  control::handle_input(session, {}, &packet, sizeof(packet));

  float TARGET_W = 1920;
  float TARGET_H = 1080;
  {
    event = get_event(li);
    REQUIRE(event);
    REQUIRE(libinput_event_get_type(event.get()) == LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY);
    auto t_event = libinput_event_get_tablet_tool_event(event.get());
    REQUIRE(libinput_event_tablet_tool_get_proximity_state(t_event) == LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_IN);
    REQUIRE(libinput_tablet_tool_get_type(libinput_event_tablet_tool_get_tool(t_event)) ==
            LIBINPUT_TABLET_TOOL_TYPE_PEN);
    REQUIRE(libinput_event_tablet_tool_get_distance(t_event) == 0.5);
    REQUIRE(libinput_event_tablet_tool_get_pressure(t_event) == 0.0);
    REQUIRE_THAT(libinput_event_tablet_tool_get_x_transformed(t_event, TARGET_W), WithinRel(TARGET_W * 0.5f, 0.5f));
    REQUIRE_THAT(libinput_event_tablet_tool_get_y_transformed(t_event, TARGET_H), WithinRel(TARGET_H * 0.5f, 0.5f));
    REQUIRE_THAT(libinput_event_tablet_tool_get_tilt_x(t_event), WithinRel(-45.0f, 0.1f));
    REQUIRE_THAT(libinput_event_tablet_tool_get_tilt_y(t_event),
                 WithinRel(0.0f, 0.1f)); // 90° rotation means 0° tilt Y (full right position)
    REQUIRE(libinput_event_tablet_tool_get_tip_state(t_event) == LIBINPUT_TABLET_TOOL_TIP_UP);
  }

  packet = pkts::PEN_PACKET{.event_type = pkts::TOUCH_EVENT_DOWN,
                            .tool_type = pkts::TOOL_TYPE_PEN,
                            .pen_buttons = pkts::PEN_BUTTON_TYPE_PRIMARY,
                            .x = {0, 0, 0, 63},                    // 0.5 in float (little endian)
                            .y = {0, 0, 0, 63},                    // 0.5 in float (little endian)
                            .pressure_or_distance = {0, 0, 0, 63}, // 0.5 in float (little endian)
                            .rotation = 180,
                            .tilt = 90};
  packet.type = pkts::PEN;

  control::handle_input(session, {}, &packet, sizeof(packet));
  {
    event = get_event(li);
    REQUIRE(libinput_event_get_type(event.get()) == LIBINPUT_EVENT_TABLET_TOOL_TIP);
    auto t_event = libinput_event_get_tablet_tool_event(event.get());
    REQUIRE(libinput_event_tablet_tool_get_proximity_state(t_event) == LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_IN);
    REQUIRE(libinput_tablet_tool_get_type(libinput_event_tablet_tool_get_tool(t_event)) ==
            LIBINPUT_TABLET_TOOL_TYPE_PEN);
    REQUIRE(libinput_event_tablet_tool_get_distance(t_event) == 0.0);
    REQUIRE_THAT(libinput_event_tablet_tool_get_pressure(t_event), WithinRel(0.5f, 0.1f));
    REQUIRE_THAT(libinput_event_tablet_tool_get_x_transformed(t_event, TARGET_W), WithinRel(TARGET_W * 0.5f, 0.5f));
    REQUIRE_THAT(libinput_event_tablet_tool_get_y_transformed(t_event, TARGET_H), WithinRel(TARGET_H * 0.5f, 0.5f));
    REQUIRE_THAT(libinput_event_tablet_tool_get_tilt_x(t_event), WithinRel(90.0f, 0.1f));
    REQUIRE_THAT(libinput_event_tablet_tool_get_tilt_y(t_event),
                 WithinRel(-90.0f, 0.1f)); // 180° rotation means -90° tilt Y (full down position)
    REQUIRE(libinput_event_tablet_tool_get_tip_state(t_event) == LIBINPUT_TABLET_TOOL_TIP_DOWN);
  }
}

TEST_CASE("uinput - touch screen", "[UINPUT]") {
  auto session = events::StreamSession{.event_bus = std::make_shared<events::EventBusType>()};

  auto packet = pkts::TOUCH_PACKET{
      .event_type = pkts::TOUCH_EVENT_UP,
      .pointer_id = boost::endian::native_to_little(0),
      .x = {0, 0, 0, 63},                    // 0.5 in float (little endian)
      .y = {0, 0, 0, 63},                    // 0.5 in float (little endian)
      .pressure_or_distance = {0, 0, 0, 63}, // 0.5 in float (little endian)
  };
  packet.type = pkts::TOUCH;

  control::handle_input(session, {}, &packet, sizeof(packet)); // Should create a touch screen when a packet arrives
  REQUIRE(session.touch_screen->has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  auto li = create_libinput_context(std::get<TouchScreen>(session.touch_screen->value()).get_nodes());
  auto event = get_event(li);
  REQUIRE(event);
  REQUIRE(libinput_event_get_type(event.get()) == LIBINPUT_EVENT_DEVICE_ADDED);
  REQUIRE(libinput_device_has_capability(libinput_event_get_device(event.get()), LIBINPUT_DEVICE_CAP_TOUCH));

  packet.event_type = pkts::TOUCH_EVENT_HOVER;
  control::handle_input(session, {}, &packet, sizeof(packet));

  float TARGET_W = 1920;
  float TARGET_H = 1080;
  {
    event = get_event(li);
    REQUIRE(libinput_event_get_type(event.get()) == LIBINPUT_EVENT_TOUCH_DOWN);
    auto t_event = libinput_event_get_touch_event(event.get());
    REQUIRE(libinput_event_touch_get_slot(t_event) == 1);
    REQUIRE_THAT(libinput_event_touch_get_x_transformed(t_event, TARGET_W), WithinRel(TARGET_W * 0.5f, 0.5f));
    REQUIRE_THAT(libinput_event_touch_get_y_transformed(t_event, TARGET_H), WithinRel(TARGET_H * 0.5f, 0.5f));
    event = get_event(li);
    REQUIRE(libinput_event_get_type(event.get()) == LIBINPUT_EVENT_TOUCH_FRAME);
  }

  packet = pkts::TOUCH_PACKET{
      .event_type = pkts::TOUCH_EVENT_UP,
      .pointer_id = boost::endian::native_to_little(0),
  };
  packet.type = pkts::TOUCH;

  control::handle_input(session, {}, &packet, sizeof(packet));

  {
    event = get_event(li);
    REQUIRE(libinput_event_get_type(event.get()) == LIBINPUT_EVENT_TOUCH_UP);
    auto t_event = libinput_event_get_touch_event(event.get());
    REQUIRE(libinput_event_touch_get_slot(t_event) == 1);
    event = get_event(li);
    REQUIRE(libinput_event_get_type(event.get()) == LIBINPUT_EVENT_TOUCH_FRAME);
  }
}

TEST_CASE("uinput - mouse", "[UINPUT]") {
  libevdev_ptr mouse_rel_dev(libevdev_new(), ::libevdev_free);
  libevdev_ptr mouse_abs_dev(libevdev_new(), ::libevdev_free);
  auto mouse = std::make_shared<std::optional<events::MouseTypes>>(std::move(*Mouse::create()));
  auto session = events::StreamSession{.display_mode = {.width = 1920, .height = 1080}, .mouse = mouse};

  link_devnode(mouse_rel_dev.get(), std::get<state::input::Mouse>(mouse->value()).get_nodes()[0]);
  link_devnode(mouse_abs_dev.get(), std::get<state::input::Mouse>(mouse->value()).get_nodes()[1]);

  auto events = fetch_events_debug(mouse_rel_dev);
  REQUIRE(events.empty());
  events = fetch_events_debug(mouse_abs_dev);
  REQUIRE(events.empty());

  SECTION("Mouse move") {
    auto mv_packet = pkts::MOUSE_MOVE_REL_PACKET{.delta_x = 10, .delta_y = 20};
    mv_packet.type = pkts::MOUSE_MOVE_REL;

    control::handle_input(session, {}, &mv_packet, sizeof(mv_packet));
    events = fetch_events_debug(mouse_rel_dev);
    REQUIRE(events.size() == 2);
    REQUIRE_THAT(libevdev_event_type_get_name(events[0]->type), Equals("EV_REL"));
    REQUIRE_THAT(libevdev_event_code_get_name(events[0]->type, events[0]->code), Equals("REL_X"));
    REQUIRE(events[0]->value > 0);

    REQUIRE_THAT(libevdev_event_type_get_name(events[1]->type), Equals("EV_REL"));
    REQUIRE_THAT(libevdev_event_code_get_name(events[1]->type, events[1]->code), Equals("REL_Y"));
    REQUIRE(events[1]->value > 0);
  }

  SECTION("Mouse move absolute") {
    auto mv_packet = pkts::MOUSE_MOVE_ABS_PACKET{.x = boost::endian::native_to_big((short)100),
                                                 .y = boost::endian::native_to_big((short)200),
                                                 .width = boost::endian::native_to_big((short)1280),
                                                 .height = boost::endian::native_to_big((short)720)};
    mv_packet.type = pkts::MOUSE_MOVE_ABS;

    control::handle_input(session, {}, &mv_packet, sizeof(mv_packet));
    events = fetch_events_debug(mouse_abs_dev);
    REQUIRE(events.size() == 2);
    REQUIRE_THAT(libevdev_event_type_get_name(events[0]->type), Equals("EV_ABS"));
    REQUIRE_THAT(libevdev_event_code_get_name(events[0]->type, events[0]->code), Equals("ABS_X"));
    REQUIRE(events[0]->value > 0);

    REQUIRE_THAT(libevdev_event_type_get_name(events[1]->type), Equals("EV_ABS"));
    REQUIRE_THAT(libevdev_event_code_get_name(events[1]->type, events[1]->code), Equals("ABS_Y"));
    REQUIRE(events[1]->value > 0);
  }

  SECTION("Mouse press button") {
    auto pressed_packet = pkts::MOUSE_BUTTON_PACKET{.button = 5};
    pressed_packet.type = pkts::MOUSE_BUTTON_PRESS;

    control::handle_input(session, {}, &pressed_packet, sizeof(pressed_packet));
    events = fetch_events_debug(mouse_rel_dev);
    REQUIRE(events.size() == 2);
    REQUIRE_THAT(libevdev_event_type_get_name(events[0]->type), Equals("EV_MSC"));
    REQUIRE_THAT(libevdev_event_code_get_name(events[0]->type, events[0]->code), Equals("MSC_SCAN"));
    REQUIRE(events[0]->value == 90005);

    REQUIRE_THAT(libevdev_event_type_get_name(events[1]->type), Equals("EV_KEY"));
    REQUIRE_THAT(libevdev_event_code_get_name(events[1]->type, events[1]->code), Equals("BTN_EXTRA"));
    REQUIRE(events[1]->value == 1);
  }

  SECTION("Mouse scroll") {
    short scroll_amt = 10;
    auto scroll_packet = pkts::MOUSE_SCROLL_PACKET{.scroll_amt1 = boost::endian::native_to_big(scroll_amt)};
    scroll_packet.type = pkts::MOUSE_SCROLL;

    control::handle_input(session, {}, &scroll_packet, sizeof(scroll_packet));
    events = fetch_events_debug(mouse_rel_dev);
    REQUIRE(events.size() == 1);
    REQUIRE_THAT(libevdev_event_type_get_name(events[0]->type), Equals("EV_REL"));
    REQUIRE_THAT(libevdev_event_code_get_name(events[0]->type, events[0]->code), Equals("REL_WHEEL_HI_RES"));
    REQUIRE(events[0]->value == scroll_amt);
  }

  SECTION("Mouse horizontal scroll") {
    short scroll_amt = 10;
    auto scroll_packet = pkts::MOUSE_HSCROLL_PACKET{.scroll_amount = boost::endian::native_to_big(scroll_amt)};
    scroll_packet.type = pkts::MOUSE_HSCROLL;

    control::handle_input(session, {}, &scroll_packet, sizeof(scroll_packet));
    events = fetch_events_debug(mouse_rel_dev);
    REQUIRE(events.size() == 1);
    REQUIRE_THAT(libevdev_event_type_get_name(events[0]->type), Equals("EV_REL"));
    REQUIRE_THAT(libevdev_event_code_get_name(events[0]->type, events[0]->code), Equals("REL_HWHEEL_HI_RES"));
    REQUIRE(events[0]->value == scroll_amt);
  }

  SECTION("UDEV") {
    auto udev_events = std::get<state::input::Mouse>(mouse->value()).get_udev_events();

    REQUIRE(udev_events.size() == 2);

    REQUIRE_THAT(udev_events[0]["ACTION"], Equals("add"));
    REQUIRE_THAT(udev_events[0]["ID_INPUT_MOUSE"], Equals("1"));
    REQUIRE_THAT(udev_events[0][".INPUT_CLASS"], Equals("mouse"));
    REQUIRE_THAT(udev_events[0]["DEVNAME"], ContainsSubstring("/dev/input/"));
    REQUIRE_THAT(udev_events[0]["DEVPATH"], StartsWith("/devices/virtual/input/input"));

    REQUIRE_THAT(udev_events[1]["ACTION"], Equals("add"));
    REQUIRE_THAT(udev_events[1]["ID_INPUT_TOUCHPAD"], Equals("1"));
    REQUIRE_THAT(udev_events[1][".INPUT_CLASS"], Equals("mouse"));
    REQUIRE_THAT(udev_events[1]["DEVNAME"], ContainsSubstring("/dev/input/"));
    REQUIRE_THAT(udev_events[1]["DEVPATH"], StartsWith("/devices/virtual/input/input"));
  }
}

TEST_CASE("uinput - joypad", "[UINPUT]") {
  SECTION("OLD Moonlight: create joypad on first packet arrival") {
    events::App app = {};
    auto session = events::StreamSession{.event_bus = std::make_shared<events::EventBusType>(),
                                         .app = std::make_shared<events::App>(app)};
    short controller_number = 1;
    auto c_pkt =
        pkts::CONTROLLER_MULTI_PACKET{.controller_number = controller_number, .button_flags = pkts::RIGHT_STICK};
    c_pkt.type = pkts::CONTROLLER_MULTI;

    control::handle_input(session, {}, &c_pkt, sizeof(c_pkt));

    REQUIRE(session.joypads->load()->size() == 1);
    auto joypad = session.joypads->load()->at(controller_number);
    std::visit([](auto &joypad) { REQUIRE(joypad.get_nodes().size() == 2); }, *joypad);
  }

  SECTION("NEW Moonlight: create joypad with CONTROLLER_ARRIVAL") {
    events::App app = {};
    auto session = events::StreamSession{.event_bus = std::make_shared<events::EventBusType>(),
                                         .app = std::make_shared<events::App>(app)};
    uint8_t controller_number = 1;
    auto c_pkt = pkts::CONTROLLER_ARRIVAL_PACKET{.controller_number = controller_number,
                                                 .controller_type = pkts::XBOX,
                                                 .capabilities = pkts::ANALOG_TRIGGERS};
    c_pkt.type = pkts::CONTROLLER_ARRIVAL;

    control::handle_input(session, {}, &c_pkt, sizeof(c_pkt));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto joypad = session.joypads->load()->at(controller_number);
    std::vector<std::string> dev_nodes;
    std::visit([&dev_nodes](auto &joypad) { dev_nodes = joypad.get_nodes(); }, *joypad);
    REQUIRE(session.joypads->load()->size() == 1);
    REQUIRE(dev_nodes.size() >= 2);

    // TODO: test pressing buttons

    { // UDEV
      std::vector<std::map<std::string, std::string>> udev_events;
      std::visit([&udev_events](auto &joypad) { udev_events = joypad.get_udev_events(); }, *joypad);

      for (auto event : udev_events) {
        std::stringstream ss;
        for (auto [key, value] : event) {
          ss << key << "=" << value << ", ";
        }
        logs::log(logs::debug, "UDEV: {}", ss.str());
      }

      REQUIRE(udev_events.size() == 2);

      for (auto &event : udev_events) {
        REQUIRE_THAT(event["ACTION"], Equals("add"));
        REQUIRE_THAT(event["DEVNAME"], ContainsSubstring("/dev/input/"));
        REQUIRE_THAT(event["DEVPATH"], StartsWith("/devices/virtual/input/input"));
        REQUIRE_THAT(event[".INPUT_CLASS"], StartsWith("joystick"));
      }
    }
  }
}

TEST_CASE("uinput - paste UTF8", "[UINPUT]") {

  SECTION("UTF8 to HEX") {
    auto utf8 = boost::locale::conv::to_utf<wchar_t>("\xF0\x9F\x92\xA9", "UTF-8"); // UTF-8 '💩'
    auto utf32 = boost::locale::conv::utf_to_utf<char32_t>(utf8);
    REQUIRE_THAT(wolf::platforms::input::to_hex(utf32), Equals("1F4A9"));
  }

  SECTION("UTF16 to HEX") {
    char16_t payload[] = {0xD83D, 0xDCA9}; // UTF-16 '💩'
    auto utf16 = std::u16string(payload, 2);
    auto utf32 = boost::locale::conv::utf_to_utf<char32_t>(utf16);
    REQUIRE_THAT(wolf::platforms::input::to_hex(utf32), Equals("1F4A9"));
  }

  SECTION("UTF8 claiming more text than was received is dropped") {
    libevdev_ptr keyboard_dev(libevdev_new(), ::libevdev_free);
    auto session = events::StreamSession{
        .keyboard = std::make_shared<std::optional<events::KeyboardTypes>>(std::move(*Keyboard::create()))};
    link_devnode(keyboard_dev.get(), std::get<state::input::Keyboard>(session.keyboard->value()).get_nodes()[0]);

    // Header plus 2 bytes of text, heap-allocated at its exact size, while data_size claims 4 bytes of text
    auto utf8_pkt = pkts::UTF8_TEXT_PACKET{.text = "\xF0\x9F\x92\xA9"};
    utf8_pkt.type = pkts::UTF8_TEXT;
    utf8_pkt.data_size = boost::endian::native_to_big(8);
    std::vector<char> buf(sizeof(pkts::INPUT_PKT) + 2);
    std::memcpy(buf.data(), &utf8_pkt, buf.size());

    control::handle_input(session, {}, reinterpret_cast<pkts::INPUT_PKT *>(buf.data()), buf.size());
    REQUIRE(fetch_events_debug(keyboard_dev).empty());
  }

  SECTION("Paste UTF8") {
    libevdev_ptr keyboard_dev(libevdev_new(), ::libevdev_free);
    auto session = events::StreamSession{
        .keyboard = std::make_shared<std::optional<events::KeyboardTypes>>(std::move(*Keyboard::create()))};
    link_devnode(keyboard_dev.get(), std::get<state::input::Keyboard>(session.keyboard->value()).get_nodes()[0]);

    auto events = fetch_events_debug(keyboard_dev);
    REQUIRE(events.empty());

    auto utf8_pkt = pkts::UTF8_TEXT_PACKET{.text = "\xF0\x9F\x92\xA9"};
    utf8_pkt.type = pkts::UTF8_TEXT;
    utf8_pkt.data_size = boost::endian::native_to_big(8);

    control::handle_input(session, {}, &utf8_pkt, sizeof(utf8_pkt));
    events = fetch_events_debug(keyboard_dev);
    REQUIRE(events.size() == 16);

    /**
     * Lambda, checks that the given key_name has been correctly sent via evdev
     */
    auto ev_idx = 0;
    auto require_ev = [&](const std::string &key_name, bool pressed = true) {
      REQUIRE_THAT(libevdev_event_code_get_name(events[ev_idx]->type, events[ev_idx]->code), Equals(key_name));
      REQUIRE(events[ev_idx]->value == (pressed ? 1 : 0));
      ev_idx++;
    };

    /*
     * Pressing <CTRL> + <SHIFT> + U
     */
    require_ev("KEY_LEFTCTRL");
    require_ev("KEY_LEFTSHIFT");
    require_ev("KEY_U");
    require_ev("KEY_U", false); // release U

    /*
     * At this point we should have typed: U+1F4A9
     * (twice each because it's <press>, <release>
     */
    require_ev("KEY_1");
    require_ev("KEY_1", false);
    require_ev("KEY_F");
    require_ev("KEY_F", false);
    require_ev("KEY_4");
    require_ev("KEY_4", false);
    require_ev("KEY_A");
    require_ev("KEY_A", false);
    require_ev("KEY_9");
    require_ev("KEY_9", false);

    /*
     * Finally, releasing <CTRL> and <SHIFT>
     */
    require_ev("KEY_LEFTSHIFT", false);
    require_ev("KEY_LEFTCTRL", false);
  }
}

TEST_CASE("uinput - joypads kept from a lobby are replaced or released unless the client claims them", "[UINPUT]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto session = events::StreamSession{.event_bus = event_bus, .session_id = 1234};
  const std::string lobby_id = "game-lobby";

  // The stream took over two Xbox pads kept by the lobby, still plugged into its app (sessions/add)
  std::vector<std::shared_ptr<events::JoypadTypes>> kept;
  for (int nr = 0; nr < 2; nr++) {
    auto created = inputtino::XboxOneJoypad::create();
    REQUIRE(created);
    kept.push_back(std::make_shared<events::JoypadTypes>(XboxOneJoypad(std::move(*created))));
    session.joypads->update([&](auto joypads) { return joypads.set(nr, kept.back()); });
    session.kept_joypads->plugged_into[nr] = lobby_id;
    session.kept_joypads->unclaimed.insert(nr);
  }

  std::vector<std::string> plugged, unplugged; // session ids the events are addressed to
  auto plug_observer = event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [&](const immer::box<events::PlugDeviceEvent> &ev) { plugged.push_back(ev->session_id); });
  auto unplug_observer = event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
      [&](const immer::box<events::UnplugDeviceEvent> &ev) { unplugged.push_back(ev->session_id); });
  auto arrival = [&](uint8_t controller_number, pkts::CONTROLLER_TYPE type) {
    auto c_pkt = pkts::CONTROLLER_ARRIVAL_PACKET{.controller_number = controller_number,
                                                 .controller_type = type,
                                                 .capabilities = pkts::ANALOG_TRIGGERS};
    c_pkt.type = pkts::CONTROLLER_ARRIVAL;
    control::handle_input(session, {}, &c_pkt, sizeof(c_pkt));
  };

  SECTION("The same controller arriving keeps its pad") {
    arrival(0, pkts::XBOX);
    REQUIRE(session.joypads->load()->at(0) == kept[0]);
    REQUIRE(plugged.empty());
    REQUIRE(unplugged.empty());

    // The one it didn't bring is unplugged from the lobby's app it's plugged into
    control::release_unclaimed_joypads(session);
    REQUIRE(unplugged == std::vector<std::string>{lobby_id});
    REQUIRE(session.joypads->load()->size() == 1);
    REQUIRE(!session.joypads->load()->find(1));
    REQUIRE(session.kept_joypads->unclaimed.empty());

    // Once claimed, the input's unplug (Moonlight's mask) still goes to where the pad is plugged in
    unplugged.clear();
    auto c_pkt = pkts::CONTROLLER_MULTI_PACKET{.controller_number = 0, .active_gamepad_mask = 0};
    c_pkt.type = pkts::CONTROLLER_MULTI;
    control::handle_input(session, {}, &c_pkt, sizeof(c_pkt));
    REQUIRE(unplugged == std::vector<std::string>{lobby_id});
    REQUIRE(session.joypads->load()->empty());
  }

  SECTION("Once the session joined a lobby, the unplug follows the session") {
    session.kept_joypads->plugged_into.clear(); // what the join does (sessions/lobbies.cpp)
    control::release_unclaimed_joypads(session);
    REQUIRE(unplugged ==
            std::vector<std::string>{std::to_string(session.session_id), std::to_string(session.session_id)});
    REQUIRE(session.joypads->load()->empty());
  }

  SECTION("The release waits for the timeout") {
    auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>(
        immer::vector<events::StreamSession>{session});
    control::release_unclaimed_joypads_after(running_sessions, session, std::chrono::milliseconds(200));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(unplugged.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE(unplugged == std::vector<std::string>{lobby_id, lobby_id});
    REQUIRE(session.joypads->load()->empty());
  }

  SECTION("A stream no longer running releases nothing") {
    auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
    control::release_unclaimed_joypads_after(running_sessions, session, std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(unplugged.empty());
    REQUIRE(session.joypads->load()->size() == 2);
  }

  SECTION("Another type of controller replaces the pad") {
    arrival(1, pkts::NINTENDO);
    REQUIRE(unplugged == std::vector<std::string>{lobby_id});
    REQUIRE(plugged == std::vector<std::string>{std::to_string(session.session_id)});
    auto replaced = session.joypads->load()->at(1);
    REQUIRE(std::holds_alternative<SwitchJoypad>(*replaced));

    // Claimed now: a second arrival (or the release) leaves the new pad alone
    arrival(1, pkts::XBOX);
    REQUIRE(session.joypads->load()->at(1) == replaced);
    unplugged.clear();
    arrival(0, pkts::XBOX);
    control::release_unclaimed_joypads(session);
    REQUIRE(unplugged.empty());
    REQUIRE(session.joypads->load()->size() == 2);

    // The new pad was plugged in through the session: so is its unplug, while the old one stays in the lobby
    auto c_pkt = pkts::CONTROLLER_MULTI_PACKET{.controller_number = 1, .active_gamepad_mask = 0b01};
    c_pkt.type = pkts::CONTROLLER_MULTI;
    control::handle_input(session, {}, &c_pkt, sizeof(c_pkt));
    REQUIRE(unplugged == std::vector<std::string>{std::to_string(session.session_id)});
    c_pkt.controller_number = 0;
    c_pkt.active_gamepad_mask = 0;
    control::handle_input(session, {}, &c_pkt, sizeof(c_pkt));
    REQUIRE(unplugged == std::vector<std::string>{std::to_string(session.session_id), lobby_id});
  }

  SECTION("A kept pad's first input claims it (old Moonlight sends no arrival)") {
    for (short nr = 0; nr < 2; nr++) {
      auto c_pkt = pkts::CONTROLLER_MULTI_PACKET{.controller_number = nr, .active_gamepad_mask = 0b11};
      c_pkt.type = pkts::CONTROLLER_MULTI;
      control::handle_input(session, {}, &c_pkt, sizeof(c_pkt));
    }
    control::release_unclaimed_joypads(session);
    REQUIRE(unplugged.empty());
    REQUIRE(session.joypads->load()->at(0) == kept[0]);
    REQUIRE(session.joypads->load()->at(1) == kept[1]);
  }

  SECTION("A pad the stream created itself is never replaced") {
    session.kept_joypads->unclaimed.clear();
    arrival(0, pkts::NINTENDO);
    REQUIRE(session.joypads->load()->at(0) == kept[0]);
    REQUIRE(unplugged.empty());
  }
}
