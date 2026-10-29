#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::Equals;

#include <atomic>
#include <control/control.hpp>
#include <moonlight/control.hpp>
#include <sys/socket.h>
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

TEST_CASE("control joypad input packets") {
  std::string payload =
      crypto::hex_to_str("060222000000001E0C0000001A000000010014000010000000000000000000009C0000005500");

  auto input_data = (pkts::CONTROLLER_MULTI_PACKET *)payload.data();
  auto pressed_btns = input_data->button_flags | (input_data->buttonFlags2 << 16);

  REQUIRE(input_data->type == pkts::CONTROLLER_MULTI);
  REQUIRE(input_data->active_gamepad_mask == 1);
  REQUIRE(pressed_btns & pkts::CONTROLLER_BTN::A);
}
TEST_CASE("Control: concurrent sends to a connected client", "CONTROL") {
  constexpr std::uint16_t port = 48555;
  constexpr std::uint32_t secret = 0xC0FFEE;
  constexpr std::size_t session_id = 1234;
  constexpr int n_threads = 8;
  constexpr int pkts_per_thread = 50;
  std::string aes_key = "EDF04A215C4FBEA20934120C8480D855";

  // run_control() never returns, so whatever it references is leaked on purpose to outlive this test
  auto sessions = new state::SessionsAtoms(std::make_shared<immer::atom<immer::vector<events::StreamSession>>>(
      immer::vector<events::StreamSession>{events::StreamSession{.aes_key = aes_key,
                                                                 .enet_secret_payload = secret,
                                                                 .session_id = session_id,
                                                                 .ip = "127.0.0.1"}}));
  auto event_bus = new std::shared_ptr<events::EventBusType>(std::make_shared<events::EventBusType>());

  auto server_connected = std::make_shared<std::atomic<bool>>(false);
  auto resume_handler =
      (*event_bus)
          ->register_handler<immer::box<events::ResumeStreamEvent>>(
              [server_connected](const immer::box<events::ResumeStreamEvent> &) { *server_connected = true; });

  std::thread([sessions, event_bus]() {
    control::run_control(port, *sessions, *event_bus, 20, 50ms, "127.0.0.1");
  }).detach();

  auto client = enet_host_create(AF_INET, nullptr, 1, 0, 0, 0);
  REQUIRE(client);
  ENetAddress addr;
  enet_address_set_host(&addr, "127.0.0.1");
  enet_address_set_port(&addr, port);
  REQUIRE(enet_host_connect(client, &addr, 1, secret));

  ENetEvent event;
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!*server_connected && std::chrono::steady_clock::now() < deadline) {
    enet_host_service(client, &event, 10);
  }
  REQUIRE(*server_connected);

  // Rumble/LED callbacks and StopStreamEvent send from their own threads while the control thread services the host
  std::vector<std::thread> senders;
  for (int i = 0; i < n_threads; i++) {
    senders.emplace_back([event_bus]() {
      for (int j = 0; j < pkts_per_thread; j++) {
        (*event_bus)
            ->fire_event(immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session_id}));
      }
    });
  }

  int received = 0;
  deadline = std::chrono::steady_clock::now() + 10s;
  while (received < n_threads * pkts_per_thread && std::chrono::steady_clock::now() < deadline) {
    if (enet_host_service(client, &event, 10) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
      // CHECK, not REQUIRE: throwing here would skip joining the sender threads
      try {
        auto decrypted = decrypt_packet(*(ControlEncryptedPacket *)event.packet->data, aes_key);
        CHECK(((ControlPacket *)decrypted.data())->type == TERMINATION);
      } catch (std::runtime_error &e) {
        FAIL_CHECK(e.what());
      }
      enet_packet_destroy(event.packet);
      received++;
    }
  }
  for (auto &t : senders) {
    t.join();
  }
  REQUIRE(received == n_threads * pkts_per_thread);

  resume_handler.unregister();
  enet_host_destroy(client);
}
