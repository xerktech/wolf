#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::Equals;

#include <control/control.hpp>
#include <control/input_handler.hpp>
#include <cstring>
#include <future>
#include <moonlight/control.hpp>
#include <mutex>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>
#include <thread>
#include <vector>
using namespace moonlight::control;
using namespace std::chrono_literals;

static std::string to_string(const ControlEncryptedPacket &packet) {
  return {(char *)&packet, packet.full_size()};
}

TEST_CASE("Control AES Encryption", "CONTROL") {
  // A bunch of packets taken from a real session

  std::string aes_key = "EDF04A215C4FBEA20934120C8480D855";

  SECTION("30 bytes") { // original packet: 01001A0000000000BF0EB6DA10E47C702EC8644EB87D9CF7B6FAC9FF75CA
    std::string payload = crypto::hex_to_str("020302000000");
    std::uint32_t seq = 0;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("01001A0000000000BF0EB6DA10E47C702EC8644EB87D9CF7B6FAC9FF75CA"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("IDR_FRAME"));

    encrypted_packet.gcm_tag[0] ^= 1;
    REQUIRE_THROWS_AS(decrypt_packet(encrypted_packet, aes_key), std::runtime_error);
  }

  SECTION("29 bytes") { // original packet: 010019000100000021DBB8DC0590AF3A2B20BCE5A347DE31D366E5B9C5"
    std::string payload = crypto::hex_to_str("0703010000");
    std::uint32_t seq = 1;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("010019000100000021DBB8DC0590AF3A2B20BCE5A347DE31D366E5B9C5"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("START_B"));
  }

  SECTION("36 bytes") { // original packet: 0100200002000000220722FBADED58A03F2E8898F0F1DCB7C93F6235590618E4186AD990
    std::string payload = crypto::hex_to_str("000208000400000000000000");
    std::uint32_t seq = 2;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("0100200002000000220722FBADED58A03F2E8898F0F1DCB7C93F6235590618E4186AD990"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("PERIODIC_PING"));
  }

  SECTION("46 bytes") { // original packet:
                        // 01002A00060000005A4D999FB2542F85BDD39D99F77EB825254569D2C04E21241B5CEC01BD3F93129718ECC1F153
    std::string payload = crypto::hex_to_str("060212000000000E05000000033400C00000059F0329");
    std::uint32_t seq = 6;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(
        crypto::str_to_hex(to_string(encrypted_packet)),
        Equals("01002A00060000005A4D999FB2542F85BDD39D99F77EB825254569D2C04E21241B5CEC01BD3F93129718ECC1F153"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("INPUT_DATA"));
  }
}

TEST_CASE("Control decrypt reads only the received packet", "CONTROL") {
  std::string aes_key = "EDF04A215C4FBEA20934120C8480D855";
  std::string payload = crypto::hex_to_str("020302000000");

  SECTION("Any GCM tag, with or without NUL bytes, decrypts") {
    bool tested_nul_tag = false;
    for (std::uint32_t seq = 0; seq < 256; seq++) {
      auto encrypted = encrypt_packet(aes_key, seq, payload);
      tested_nul_tag |= std::string_view{encrypted->gcm_tag, GCM_TAG_SIZE}.find('\0') != std::string_view::npos;

      // ENet hands us a heap buffer of exactly the received size: under ASan any read past it aborts
      auto size = encrypted->full_size();
      auto received = std::make_unique<char[]>(size);
      std::memcpy(received.get(), encrypted.get(), size);
      auto &packet = *(ControlEncryptedPacket *)received.get();

      REQUIRE(packet.fits_in(size));
      REQUIRE_THAT(decrypt_packet(packet, aes_key), Equals(payload));
    }
    REQUIRE(tested_nul_tag);
  }

  SECTION("Packets shorter than their header claims are rejected") {
    auto encrypted = encrypt_packet(aes_key, 0, payload);
    auto size = encrypted->full_size();
    REQUIRE(encrypted->fits_in(size));
    REQUIRE_FALSE(encrypted->fits_in(size - 1));
    REQUIRE_FALSE(encrypted->fits_in(sizeof(ControlPacket)));

    encrypted->header.length = boost::endian::native_to_little<std::uint16_t>(sizeof(std::uint32_t) + GCM_TAG_SIZE - 1);
    REQUIRE_FALSE(encrypted->fits_in(size));
  }
}

TEST_CASE("Control replay window", "CONTROL") {
  auto accept_all = [](std::initializer_list<std::uint32_t> seqs) {
    ReplayWindow window;
    for (auto seq : seqs) {
      auto next = accept_seq(window, seq);
      REQUIRE(next);
      window = *next;
    }
    return window;
  };

  SECTION("in order, out of order and with gaps") {
    auto window = accept_all({0, 1, 2, 5, 3, 10, 4});
    REQUIRE(*window.highest_seq == 10);
    for (std::uint32_t seq : {0, 1, 2, 3, 4, 5, 10}) {
      REQUIRE_FALSE(accept_seq(window, seq));
    }
    // Seqs that never arrived can still come in late, once
    auto late = accept_seq(window, 7);
    REQUIRE(late);
    REQUIRE_FALSE(accept_seq(*late, 7));
  }

  SECTION("the first seq doesn't have to be 0") {
    auto window = accept_all({100});
    REQUIRE_FALSE(accept_seq(window, 100));
    REQUIRE(accept_seq(window, 99));
  }

  SECTION("too old to tell") {
    auto window = accept_all({0, ReplayWindow::SIZE});
    REQUIRE_FALSE(accept_seq(window, 0));
    REQUIRE(accept_seq(window, 1));
    window = accept_all({0, ReplayWindow::SIZE + 1});
    REQUIRE_FALSE(accept_seq(window, 1)); // never seen, but out of the window
    REQUIRE(accept_seq(window, 2));
  }

  SECTION("a jump past the whole window forgets the old seqs") {
    auto window = accept_all({5, 10'000'000, UINT32_MAX});
    REQUIRE_FALSE(accept_seq(window, UINT32_MAX));
    REQUIRE_FALSE(accept_seq(window, 10'000'000));
    REQUIRE(accept_seq(window, UINT32_MAX - 1));
  }

  SECTION("the low byte repeats every 256 packets, the full seq doesn't") {
    auto window = accept_all({1});
    REQUIRE(accept_seq(window, 257));
  }
}

TEST_CASE("control joypad input packets") {
  std::string payload =
      crypto::hex_to_str("060222000000001E0C0000001A000000010014000010000000000000000000009C0000005500");

  auto input_data = (pkts::CONTROLLER_MULTI_PACKET *)payload.data();
  auto pressed_btns = input_data->button_flags | (input_data->buttonFlags2 << 16);

  REQUIRE(input_data->type == pkts::CONTROLLER_MULTI);
  REQUIRE(input_data->active_gamepad_mask == 1);
  REQUIRE(pressed_btns & pkts::CONTROLLER_BTN::A);
}
TEST_CASE("control: a resumed stream supersedes the dead peer of the same session") {
  ENetPeer dead_peer{}, resumed_peer{}, other_client_peer{};
  auto session = [](std::size_t id) {
    return immer::box<wolf::core::events::StreamSession>(wolf::core::events::StreamSession{.session_id = id});
  };

  control::enet_clients_map clients;
  clients = clients.set(&dead_peer, session(42)).set(&other_client_peer, session(7));

  // The resumed peer connects for session 42 while the dead one is still waiting to time out
  auto stale = control::superseded_peers(clients, &resumed_peer, 42);
  REQUIRE(stale.size() == 1);
  REQUIRE(stale[0] == &dead_peer);

  // Other clients' peers are never touched
  REQUIRE(control::superseded_peers(clients, &resumed_peer, 7).size() == 1);
  REQUIRE(control::superseded_peers(clients, &other_client_peer, 7).empty());

  // Re-registering the same peer doesn't supersede itself
  REQUIRE(control::superseded_peers(clients, &dead_peer, 42).empty());
}

TEST_CASE("control: a pause from a stream replaced by /resume is ignored") {
  using wolf::core::events::StreamSession;
  StreamSession dead_stream{.session_id = 42};
  dead_stream.enet_secret_payload = 1;
  StreamSession resumed_stream = dead_stream;
  resumed_stream.enet_secret_payload = 2;
  StreamSession other_client{.session_id = 7};
  other_client.enet_secret_payload = 1;

  // Before the resume the dead stream is still the current one: its pause is real
  REQUIRE_FALSE(control::is_superseded_stream(immer::vector<StreamSession>{dead_stream, other_client}, dead_stream));

  // After the resume replaced it, the dead peer's late pause must be ignored, the new peer's must not
  immer::vector<StreamSession> running{other_client, resumed_stream};
  REQUIRE(control::is_superseded_stream(running, dead_stream));
  REQUIRE_FALSE(control::is_superseded_stream(running, resumed_stream));

  // Session already gone (e.g. cancelled): keep pausing as before
  REQUIRE_FALSE(control::is_superseded_stream(immer::vector<StreamSession>{other_client}, dead_stream));
}

namespace {
/* A Moonlight-like ENet client: connects with the session's ENet secret as connect data */
struct TestENetClient {
  ENetHost *host = enet_host_create(AF_INET, nullptr, 1, 1, 0, 0);
  ENetPeer *peer = nullptr;
  bool disconnected = false;
  std::size_t received = 0;
  std::vector<std::string> received_packets;

  TestENetClient(int port, std::uint32_t enet_secret) {
    ENetAddress addr;
    enet_address_set_host(&addr, "127.0.0.1");
    enet_address_set_port(&addr, port);
    peer = enet_host_connect(host, &addr, 1, enet_secret);
    ENetEvent ev;
    for (int i = 0; i < 200; i++) {
      if (enet_host_service(host, &ev, 10) > 0 && ev.type == ENET_EVENT_TYPE_CONNECT)
        return;
    }
    FAIL("ENet client failed to connect");
  }

  void service(std::chrono::milliseconds duration) {
    auto end = std::chrono::steady_clock::now() + duration;
    ENetEvent ev;
    while (std::chrono::steady_clock::now() < end) {
      while (enet_host_service(host, &ev, 5) > 0) {
        if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
          disconnected = true;
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
          received++;
          received_packets.emplace_back((char *)ev.packet->data, ev.packet->dataLength);
          enet_packet_destroy(ev.packet);
        }
      }
    }
  }

  void send_encrypted(const std::string &aes_key,
                      std::uint32_t seq,
                      std::string_view payload,
                      ControlEncryption encryption = ControlEncryption::V1) {
    auto encrypted = encrypt_packet(aes_key, seq, payload, encryption, ControlPacketOrigin::CLIENT);
    send_raw({(char *)encrypted.get(), encrypted->full_size()});
  }

  void send_raw(std::string_view packet) {
    enet_peer_send(peer, 0, enet_packet_create(packet.data(), packet.size(), ENET_PACKET_FLAG_RELIABLE));
    enet_host_flush(host);
  }

  void send_termination(const std::string &aes_key, std::uint32_t seq = 0) {
    auto pkt = ControlTerminatePacket{};
    send_encrypted(aes_key, seq, {(char *)&pkt, sizeof(pkt)});
  }

  void send_idr(const std::string &aes_key, std::uint32_t seq, ControlEncryption encryption) {
    auto pkt = ControlPacket{.type = pkts::IDR_FRAME, .length = 0};
    send_encrypted(aes_key, seq, {(char *)&pkt, sizeof(pkt)}, encryption);
  }

  ~TestENetClient() {
    enet_host_destroy(host);
  }
};
} // namespace

TEST_CASE("sessions: a stopped or replaced stream instance is no longer the running one") {
  using wolf::core::events::StreamSession;
  StreamSession stopped_stream{.session_id = 42};
  stopped_stream.enet_secret_payload = 1;
  StreamSession resumed_stream = stopped_stream;
  resumed_stream.enet_secret_payload = 2;
  StreamSession other_client{.session_id = 7};
  other_client.enet_secret_payload = 1;

  REQUIRE(state::is_running_instance(immer::vector<StreamSession>{other_client, stopped_stream}, stopped_stream));

  // Its runner exits after the stop removed it: nothing to stop
  REQUIRE_FALSE(state::is_running_instance(immer::vector<StreamSession>{other_client}, stopped_stream));

  // ... or after a /resume added a new instance under the same session_id: that one must not be stopped
  immer::vector<StreamSession> resumed{other_client, resumed_stream};
  REQUIRE_FALSE(state::is_running_instance(resumed, stopped_stream));
  REQUIRE(state::is_running_instance(resumed, resumed_stream));
}

TEST_CASE("control: a dead client's late pause doesn't end its resumed stream", "[ENET]") {
  using wolf::core::events::StreamSession;
  constexpr int port = 47998;
  const std::string old_key = "EDF04A215C4FBEA20934120C8480D855", new_key = "00112233445566778899AABBCCDDEEFF";
  auto make_session = [](std::uint32_t enet_secret, const std::string &aes_key) {
    StreamSession session{.session_id = 42};
    session.enet_secret_payload = enet_secret;
    session.rtp_secret_payload = {static_cast<char>(enet_secret)};
    session.aes_key = aes_key;
    session.ip = "10.255.255.1"; // never matches loopback, keep the IP fallback out of the way
    return session;
  };

  auto event_bus = std::make_shared<wolf::core::events::EventBusType>();
  std::atomic<int> pauses = 0;
  auto pause_handler = event_bus->register_handler<immer::box<wolf::core::events::PauseStreamEvent>>(
      [&pauses](const immer::box<wolf::core::events::PauseStreamEvent> &ev) { pauses++; });
  // The RTP secret (first byte) of each stream instance ended as superseded
  std::mutex ended_mutex;
  std::vector<char> ended_streams;
  auto end_handler = event_bus->register_handler<immer::box<wolf::core::events::EndSupersededStreamEvent>>(
      [&](const immer::box<wolf::core::events::EndSupersededStreamEvent> &ev) {
        std::lock_guard lock(ended_mutex);
        ended_streams.push_back(ev->rtp_secret_payload[0]);
      });
  auto ended = [&]() {
    std::lock_guard lock(ended_mutex);
    return ended_streams;
  };
  state::SessionsAtoms running_sessions =
      std::make_shared<immer::atom<immer::vector<StreamSession>>>(immer::vector{make_session(1, old_key)});

  REQUIRE(control::init());
  std::thread([=] { control::run_control(port, running_sessions, event_bus, 20, 10ms, "127.0.0.1"); }).detach();
  std::this_thread::sleep_for(200ms);

  TestENetClient dead_client(port, 1);
  dead_client.service(100ms);

  // /resume replaces the running session: same session_id, new ENet secret and AES key
  running_sessions->store(immer::vector{make_session(2, new_key)});

  // The dead client's pause raised before the resumed client connects only ends its own stream instance
  dead_client.send_termination(old_key);
  dead_client.service(200ms);
  REQUIRE(pauses == 0);
  REQUIRE(ended() == std::vector<char>{1});

  // Once the resumed client connects the dead peer is dropped, its late disconnect can't fire anything
  TestENetClient resumed_client(port, 2);
  resumed_client.service(100ms); // ack the handshake so that the server sees the connection
  dead_client.service(200ms);
  REQUIRE(dead_client.disconnected);
  REQUIRE(pauses == 0);
  REQUIRE(ended() == std::vector<char>{1, 1});

  // Stopping the stream reaches the resumed client
  event_bus->fire_event(
      immer::box<wolf::core::events::StopStreamEvent>(wolf::core::events::StopStreamEvent{.session_id = 42}));
  resumed_client.service(200ms);
  REQUIRE(resumed_client.received == 1);

  // A real pause from the current client still goes through
  resumed_client.send_termination(new_key);
  resumed_client.service(200ms);
  REQUIRE(pauses == 1);
  REQUIRE(ended() == std::vector<char>{1, 1});

  // The same stream instance reconnecting its ENet peer keeps its pipelines
  TestENetClient reconnected_client(port, 2);
  reconnected_client.service(100ms);
  resumed_client.service(200ms);
  REQUIRE(resumed_client.disconnected);
  REQUIRE(ended() == std::vector<char>{1, 1});

  // Once the stream is stopped, its client's late pause fires nothing: a /resume may be about to reuse the id
  running_sessions->store(immer::vector<StreamSession>{});
  reconnected_client.send_termination(new_key);
  reconnected_client.service(200ms);
  REQUIRE(pauses == 1);
  REQUIRE(ended() == std::vector<char>{1, 1});
}

TEST_CASE("streaming: a superseded stream instance ends only its own pipelines", "[GSTREAMER]") {
  using namespace wolf::core;
  auto event_bus = std::make_shared<events::EventBusType>();
  boost::asio::io_context io;
  auto rtp_secret = [](char id) { return std::array<char, 16>{id}; };

  // Runs a test pipeline in its own thread; returns a flag that's set once it has ended
  auto start_video = [&](std::size_t session_id, char secret) {
    events::VideoSession video{.gst_pipeline = "videotestsrc is-live=true ! fakesink", .session_id = session_id};
    video.display_mode = {.width = 64, .height = 64, .refreshRate = 30};
    video.bitrate_kbps = 1000;
    video.packet_size = 1024;
    video.rtp_secret_payload = rtp_secret(secret);
    auto ended = std::make_shared<std::atomic_bool>(false);
    std::thread([=, &io] {
      streaming::start_streaming_video(
          video,
          [] { return std::string(); },
          event_bus,
          "127.0.0.1",
          9,
          std::make_shared<immer::atom<gst_video_context::gst_context_ptr>>(),
          std::make_shared<boost::asio::ip::udp::socket>(io, boost::asio::ip::udp::v4()));
      *ended = true;
    }).detach();
    return ended;
  };
  auto start_audio = [&](std::size_t session_id, char secret) {
    events::AudioSession audio{.gst_pipeline = "audiotestsrc is-live=true ! fakesink", .session_id = session_id};
    audio.rtp_secret_payload = rtp_secret(secret);
    auto ended = std::make_shared<std::atomic_bool>(false);
    std::thread([=, &io] {
      streaming::start_streaming_audio(
          audio,
          [] { return std::string(); },
          event_bus,
          "127.0.0.1",
          9,
          std::make_shared<boost::asio::ip::udp::socket>(io, boost::asio::ip::udp::v4()),
          "sink",
          "");
      *ended = true;
    }).detach();
    return ended;
  };

  auto old_video = start_video(42, 1), old_audio = start_audio(42, 1);
  auto resumed_video = start_video(42, 2), resumed_audio = start_audio(42, 2);
  auto other_session_video = start_video(7, 1);
  std::this_thread::sleep_for(700ms);

  event_bus->fire_event(immer::box<events::EndSupersededStreamEvent>(
      events::EndSupersededStreamEvent{.session_id = 42, .rtp_secret_payload = rtp_secret(1)}));
  std::this_thread::sleep_for(1500ms);
  CHECK(*old_video);
  CHECK(*old_audio);
  CHECK_FALSE(*resumed_video);
  CHECK_FALSE(*resumed_audio);
  CHECK_FALSE(*other_session_video);

  for (std::size_t session_id : {42, 7}) {
    event_bus->fire_event(immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session_id}));
  }
  std::this_thread::sleep_for(1500ms);
  CHECK(*resumed_video);
  CHECK(*resumed_audio);
  CHECK(*other_session_video);
}

/**
 * Feeds handle_input a heap buffer of exactly `len` bytes, so that under ASan any read past it is reported
 */
static void handle_truncated(pkts::INPUT_TYPE type, std::size_t len, std::uint32_t data_size = 0) {
  std::vector<char> buf(len);
  pkts::INPUT_PKT header{.packet_type = pkts::INPUT_DATA,
                         .packet_len = boost::endian::native_to_little(static_cast<unsigned short>(len - 4)),
                         .data_size = boost::endian::native_to_big(data_size),
                         .type = type};
  std::memcpy(buf.data(), &header, std::min(len, sizeof(header)));
  auto session = wolf::core::events::StreamSession{};
  control::handle_input(session, {}, reinterpret_cast<pkts::INPUT_PKT *>(buf.data()), buf.size());
  REQUIRE(session.joypads->load()->size() == 0);
}

TEST_CASE("control input packets shorter than their type are dropped", "[CONTROL]") {
  SECTION("Shorter than the input header") {
    for (std::size_t len = 0; len < sizeof(pkts::INPUT_PKT); len++) {
      std::vector<char> buf(len);
      auto session = wolf::core::events::StreamSession{};
      control::handle_input(session, {}, reinterpret_cast<pkts::INPUT_PKT *>(buf.data()), buf.size());
    }
  }

  auto check_type = [](pkts::INPUT_TYPE type, std::size_t full_size) {
    for (std::size_t len = sizeof(pkts::INPUT_PKT); len < full_size; len++) {
      handle_truncated(type, len);
    }
  };
  SECTION("MOUSE_MOVE_REL") {
    check_type(pkts::MOUSE_MOVE_REL, sizeof(pkts::MOUSE_MOVE_REL_PACKET));
  }
  SECTION("MOUSE_MOVE_ABS") {
    check_type(pkts::MOUSE_MOVE_ABS, sizeof(pkts::MOUSE_MOVE_ABS_PACKET));
  }
  SECTION("MOUSE_BUTTON") {
    check_type(pkts::MOUSE_BUTTON_PRESS, sizeof(pkts::MOUSE_BUTTON_PACKET));
    check_type(pkts::MOUSE_BUTTON_RELEASE, sizeof(pkts::MOUSE_BUTTON_PACKET));
  }
  SECTION("MOUSE_SCROLL") {
    check_type(pkts::MOUSE_SCROLL, sizeof(pkts::MOUSE_SCROLL_PACKET));
  }
  SECTION("MOUSE_HSCROLL") {
    check_type(pkts::MOUSE_HSCROLL, sizeof(pkts::MOUSE_HSCROLL_PACKET));
  }
  SECTION("KEYBOARD") {
    check_type(pkts::KEY_PRESS, sizeof(pkts::KEYBOARD_PACKET));
    check_type(pkts::KEY_RELEASE, sizeof(pkts::KEYBOARD_PACKET));
  }
  SECTION("TOUCH") {
    check_type(pkts::TOUCH, sizeof(pkts::TOUCH_PACKET));
  }
  SECTION("PEN") {
    check_type(pkts::PEN, sizeof(pkts::PEN_PACKET));
  }
  SECTION("CONTROLLER_ARRIVAL") {
    check_type(pkts::CONTROLLER_ARRIVAL, sizeof(pkts::CONTROLLER_ARRIVAL_PACKET));
  }
  SECTION("CONTROLLER_MULTI") {
    check_type(pkts::CONTROLLER_MULTI, sizeof(pkts::CONTROLLER_MULTI_PACKET));
  }
  SECTION("CONTROLLER_TOUCH") {
    check_type(pkts::CONTROLLER_TOUCH, sizeof(pkts::CONTROLLER_TOUCH_PACKET));
  }
  SECTION("CONTROLLER_MOTION") {
    check_type(pkts::CONTROLLER_MOTION, sizeof(pkts::CONTROLLER_MOTION_PACKET));
  }
  SECTION("CONTROLLER_BATTERY") {
    check_type(pkts::CONTROLLER_BATTERY, sizeof(pkts::CONTROLLER_BATTERY_PACKET));
  }

  SECTION("UTF8_TEXT whose data_size doesn't match what was received") {
    auto header = sizeof(pkts::INPUT_PKT);
    handle_truncated(pkts::UTF8_TEXT, header, 0);              // data_size smaller than the type
    handle_truncated(pkts::UTF8_TEXT, header, 3);              // data_size smaller than the type
    handle_truncated(pkts::UTF8_TEXT, header + 2, 4 + 4);      // claims 4 bytes of text, has 2
    handle_truncated(pkts::UTF8_TEXT, header + 2, 0xFFFFFFFF); // huge data_size
    handle_truncated(pkts::UTF8_TEXT, header + 64, 4 + 64);    // longer than UTF8_TEXT_MAX_LEN
  }
}

TEST_CASE("streaming: a stream listens to its own instance's producers", "[GSTREAMER]") {
  auto listen_to = [](const std::string &configured, const std::string &producer_name) {
    auto desc = fmt::format("interpipesrc name=interpipesrc_42_video listen-to={} ! fakesink", configured);
    auto pipeline = gst_parse_launch(desc.c_str(), nullptr);
    REQUIRE(pipeline);
    streaming::listen_to_producer(pipeline, 42, producer_name, "video");
    auto src = gst_bin_get_by_name(GST_BIN(pipeline), "interpipesrc_42_video");
    gchar *value = nullptr;
    g_object_get(src, "listen-to", &value, nullptr);
    std::string result = value ? value : "";
    g_free(value);
    gst_object_unref(src);
    gst_object_unref(pipeline);
    return result;
  };

  REQUIRE(listen_to("42_video", "42_0badcafe") == "42_0badcafe_video");
  // A custom source, or no known producer, keeps the configured one
  REQUIRE(listen_to("some-lobby_video", "42_0badcafe") == "some-lobby_video");
  REQUIRE(listen_to("42_video", "") == "42_video");
}

TEST_CASE("streaming: a lobby join while a stream starts is not lost", "[GSTREAMER]") {
  using namespace wolf::core;
  std::string kind = GENERATE("video", "audio");
  auto event_bus = std::make_shared<events::EventBusType>();
  auto src_name = fmt::format("interpipesrc_42_{}", kind);
  gstreamer::gst_element_ptr pipeline(
      gst_parse_launch(fmt::format("interpipesrc name={} listen-to=42_{} ! fakesink", src_name, kind).c_str(), nullptr),
      [](GstElement *el) { gst_object_unref(el); });
  REQUIRE(pipeline);
  auto listen_to = [&] {
    auto src = gst_bin_get_by_name(GST_BIN(pipeline.get()), src_name.c_str());
    gchar *value = nullptr;
    g_object_get(src, "listen-to", &value, nullptr);
    std::string result = value ? value : "";
    g_free(value);
    gst_object_unref(src);
    return result;
  };
  auto switch_to = [&](const std::string &producer) {
    event_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>(
        events::SwitchStreamProducerEvents{.session_id = 42, .interpipe_src_id = producer}));
  };

  std::future<void> join;
  auto handler = streaming::follow_producer(
      pipeline,
      42,
      kind,
      [&] {
        // The join lands right after the stream read its producer: the switch it fires must reach this pipeline
        join = std::async(std::launch::async, [&] { switch_to("some-lobby"); });
        join.wait_for(200ms);
        return std::string("42_0badcafe");
      },
      event_bus);
  join.wait();
  REQUIRE(listen_to() == "some-lobby_" + kind);

  switch_to("42_0badcafe");
  REQUIRE(listen_to() == "42_0badcafe_" + kind);
}

TEST_CASE("sessions: a stream whose session joined a lobby listens to the lobby's producers") {
  using namespace wolf::core;
  events::StreamSession session{.session_id = 42};
  session.producer_name = "42_0badcafe";
  immer::vector<events::StreamSession> sessions{session};
  events::Lobby lobby{.id = "some-lobby"};
  immer::vector<events::Lobby> lobbies{lobby};

  REQUIRE(state::stream_producer_name(sessions, lobbies, 42) == "42_0badcafe");
  REQUIRE(state::stream_producer_name(sessions, lobbies, 7) == "");

  // Joined before the stream's pipelines start (e.g. during a /resume overlap): they must start on the lobby
  lobby.connected_sessions->update([](auto ids) { return ids.push_back(immer::box<std::string>{"42"}); });
  REQUIRE(state::stream_producer_name(sessions, lobbies, 42) == "some-lobby");

  lobby.connected_sessions->store({});
  REQUIRE(state::stream_producer_name(sessions, lobbies, 42) == "42_0badcafe");
}

TEST_CASE("sessions: a session built without a producer name keeps its id's producers") {
  wolf::core::events::StreamSession session{.session_id = 42};
  REQUIRE(state::producer_name(session) == "42");
  session.producer_name = "42_0badcafe";
  REQUIRE(state::producer_name(session) == "42_0badcafe");
}

/** The bytes of an encrypted packet with its (unauthenticated with V1) seq field rewritten */
static std::string with_seq(std::string packet, std::uint32_t seq) {
  ((ControlEncryptedPacket *)packet.data())->seq = boost::endian::native_to_little(seq);
  return packet;
}

TEST_CASE("Control V2 encryption", "CONTROL") {
  std::string aes_key = "EDF04A215C4FBEA20934120C8480D855";
  auto idr = ControlPacket{.type = pkts::IDR_FRAME, .length = 0};
  std::string payload = {(char *)&idr, sizeof(idr)};

  SECTION("matches moonlight-common-c's encryptControlMessage()") {
    std::uint32_t seq = 0x0A0B0C0D;
    // 12-byte IV: seq in little endian, zeros, then 'C' (client originated) and 'C' (control stream)
    std::string iv = {0x0D, 0x0C, 0x0B, 0x0A, 0, 0, 0, 0, 0, 0, 'C', 'C', 0, 0, 0, 0};
    auto [expected_msg, expected_tag] = crypto::aes_encrypt_gcm(payload, crypto::hex_to_str(aes_key, true), iv, 12);

    auto encrypted = encrypt_packet(aes_key, seq, payload, ControlEncryption::V2, ControlPacketOrigin::CLIENT);
    REQUIRE(boost::endian::little_to_native(encrypted->seq) == seq);
    REQUIRE_THAT(std::string(encrypted->gcm_tag, GCM_TAG_SIZE), Equals(expected_tag));
    REQUIRE_THAT(std::string(encrypted->encrypted_msg()), Equals(expected_msg));
    REQUIRE_THAT(decrypt_packet(*encrypted, aes_key, ControlEncryption::V2), Equals(payload));
  }

  SECTION("matches moonlight-common-c's decryptControlMessageToV1()") {
    std::uint32_t seq = 0x01020304;
    // Same as above, but 'H' (host originated)
    std::string iv = {0x04, 0x03, 0x02, 0x01, 0, 0, 0, 0, 0, 0, 'H', 'C', 0, 0, 0, 0};
    auto [expected_msg, expected_tag] = crypto::aes_encrypt_gcm(payload, crypto::hex_to_str(aes_key, true), iv, 12);

    auto encrypted = encrypt_packet(aes_key, seq, payload, ControlEncryption::V2);
    REQUIRE_THAT(std::string(encrypted->gcm_tag, GCM_TAG_SIZE), Equals(expected_tag));
    REQUIRE_THAT(std::string(encrypted->encrypted_msg()), Equals(expected_msg));
  }

  SECTION("the whole seq is authenticated") {
    auto encrypted = encrypt_packet(aes_key, 3, payload, ControlEncryption::V2, ControlPacketOrigin::CLIENT);
    std::string bytes = {(char *)encrypted.get(), encrypted->full_size()};
    for (std::uint32_t forged_seq : {3u + 256u, 0xFFFFFF03u, 4u}) {
      auto forged = with_seq(bytes, forged_seq);
      REQUIRE_THROWS(decrypt_packet(*(ControlEncryptedPacket *)forged.data(), aes_key, ControlEncryption::V2));
    }

    // With V1 only the low byte is: this is why V1 packets can't be checked for replays
    auto v1 = encrypt_packet(aes_key, 3, payload);
    std::string v1_forged = with_seq({(char *)v1.get(), v1->full_size()}, 3u + 256u);
    REQUIRE_THAT(decrypt_packet(*(ControlEncryptedPacket *)v1_forged.data(), aes_key), Equals(payload));
  }

  SECTION("a packet can't be reflected back to the side that sent it") {
    auto from_host = encrypt_packet(aes_key, 0, payload, ControlEncryption::V2, ControlPacketOrigin::HOST);
    REQUIRE_THROWS(decrypt_packet(*from_host, aes_key, ControlEncryption::V2, ControlPacketOrigin::CLIENT));
    REQUIRE_THAT(decrypt_packet(*from_host, aes_key, ControlEncryption::V2, ControlPacketOrigin::HOST),
                 Equals(payload));
  }
}

TEST_CASE("control: a replayed encrypted packet is dropped", "[ENET]") {
  using wolf::core::events::StreamSession;
  constexpr int port = 47997;
  const std::string old_key = "EDF04A215C4FBEA20934120C8480D855", new_key = "00112233445566778899AABBCCDDEEFF";
  auto make_session = [](std::uint32_t enet_secret, const std::string &aes_key, ControlEncryption encryption) {
    StreamSession session{.session_id = 7};
    session.enet_secret_payload = enet_secret;
    session.rtp_secret_payload = {static_cast<char>(enet_secret)};
    session.aes_key = aes_key;
    session.ip = "10.255.255.1";                          // never matches loopback, keep the IP fallback out of the way
    session.control_stream->encryption.store(encryption); // as negotiated in the RTSP ANNOUNCE
    return session;
  };

  auto event_bus = std::make_shared<wolf::core::events::EventBusType>();
  std::atomic<int> idr_requests = 0;
  auto idr_handler = event_bus->register_handler<immer::box<wolf::core::events::IDRRequestEvent>>(
      [&idr_requests](const immer::box<wolf::core::events::IDRRequestEvent> &ev) { idr_requests++; });
  state::SessionsAtoms running_sessions = std::make_shared<immer::atom<immer::vector<StreamSession>>>(
      immer::vector{make_session(1, old_key, ControlEncryption::V2)});

  REQUIRE(control::init());
  std::thread([=] { control::run_control(port, running_sessions, event_bus, 20, 10ms, "127.0.0.1"); }).detach();
  std::this_thread::sleep_for(200ms);

  TestENetClient client(port, 1);
  client.service(100ms);

  // A genuine packet, then the same bytes resent: only the first one is acted on
  auto idr = ControlPacket{.type = pkts::IDR_FRAME, .length = 0};
  auto captured =
      encrypt_packet(old_key, 3, {(char *)&idr, sizeof(idr)}, ControlEncryption::V2, ControlPacketOrigin::CLIENT);
  std::string captured_bytes = {(char *)captured.get(), captured->full_size()};
  client.send_raw(captured_bytes);
  client.service(200ms);
  REQUIRE(idr_requests == 1);
  client.send_raw(captured_bytes);
  client.service(200ms);
  REQUIRE(idr_requests == 1);

  // Rewriting its seq doesn't get it through, and doesn't move the window: genuine packets keep flowing
  client.send_raw(with_seq(captured_bytes, 3 + 256));
  client.send_raw(with_seq(captured_bytes, 0xFFFFFF03));
  client.service(200ms);
  REQUIRE(idr_requests == 1);

  // Out of order but never seen: accepted; replayed again: dropped
  client.send_idr(old_key, 1, ControlEncryption::V2);
  client.send_idr(old_key, 4, ControlEncryption::V2);
  client.send_idr(old_key, 1, ControlEncryption::V2);
  client.service(200ms);
  REQUIRE(idr_requests == 3);

  // A packet replayed from a new ENet connection of the same stream is still dropped
  TestENetClient reconnected(port, 1);
  reconnected.service(100ms);
  reconnected.send_raw(captured_bytes);
  reconnected.service(200ms);
  REQUIRE(idr_requests == 3);

  // Packets sent to the client each get their own seq, under the V2 host IV
  event_bus->fire_event(
      immer::box<wolf::core::events::StopStreamEvent>(wolf::core::events::StopStreamEvent{.session_id = 7}));
  event_bus->fire_event(
      immer::box<wolf::core::events::StopStreamEvent>(wolf::core::events::StopStreamEvent{.session_id = 7}));
  reconnected.service(200ms);
  REQUIRE(reconnected.received_packets.size() == 2);
  for (std::uint32_t seq = 0; seq < 2; seq++) {
    auto pkt = (ControlEncryptedPacket *)reconnected.received_packets[seq].data();
    REQUIRE(boost::endian::little_to_native(pkt->seq) == seq);
    auto decrypted = decrypt_packet(*pkt, old_key, ControlEncryption::V2, ControlPacketOrigin::HOST);
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("TERMINATION"));
  }

  // After /resume the client negotiates again and restarts its seq from 0 with the new key
  running_sessions->store(immer::vector{make_session(2, new_key, ControlEncryption::V2)});
  TestENetClient resumed(port, 2);
  resumed.service(100ms);
  resumed.send_idr(new_key, 0, ControlEncryption::V2);
  resumed.service(200ms);
  REQUIRE(idr_requests == 4);

  // A V1 client's seq can't be trusted: no replay check, and a forged seq can't lock it out
  running_sessions->store(immer::vector{make_session(3, old_key, ControlEncryption::V1)});
  TestENetClient v1_client(port, 3);
  v1_client.service(100ms);
  auto v1_idr = encrypt_packet(old_key, 5, {(char *)&idr, sizeof(idr)});
  v1_client.send_raw(with_seq({(char *)v1_idr.get(), v1_idr->full_size()}, 0xFFFFFF05));
  v1_client.send_idr(old_key, 6, ControlEncryption::V1);
  v1_client.send_idr(old_key, 7, ControlEncryption::V1);
  v1_client.service(200ms);
  REQUIRE(idr_requests == 7);
}
