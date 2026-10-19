#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <events/events.hpp>
#include <events/reflectors.hpp>
#include <rfl.hpp>
#include <rfl/json.hpp>

using Catch::Matchers::Equals;
using namespace wolf::core;

TEST_CASE("Serialize to JSON", "[serialization]") {
  SECTION("example from the README") {

    struct Person {
      std::string first_name;
      std::string last_name;
      int age;
    };

    const auto homer = Person{.first_name = "Homer", .last_name = "Simpson", .age = 45};

    REQUIRE_THAT(rfl::json::write(homer), Equals("{\"first_name\":\"Homer\",\"last_name\":\"Simpson\",\"age\":45}"));
  }

  SECTION("Wolf events") {
    auto event = events::PlugDeviceEvent{.session_id = "123",
                                         .udev_events = {{{"add", "usb"}}},
                                         .udev_hw_db_entries = {{"usb", {"usb1", "usb2"}}}};

    REQUIRE_THAT(rfl::json::write(event),
                 Equals("{\"session_id\":\"123\","
                        "\"udev_events\":[{\"add\":\"usb\"}],"
                        "\"udev_hw_db_entries\":[[\"usb\",[\"usb1\",\"usb2\"]]]}"));

    auto event2 = events::PairSignal{.client_ip = "192.168.1.1", .host_ip = "0.0.0.0"};

    REQUIRE_THAT(rfl::json::write(event2),
                 Equals("{\"client_ip\":\"192.168.1.1\","
                        "\"host_ip\":\"0.0.0.0\"}"));
  }

  // TODO: test the inverse operation, rfl::json::read
}

TEST_CASE("JSON integers that don't fit the field are rejected", "[serialization]") {
  auto read_video = [](const std::string &width) {
    return rfl::json::read<events::VideoSettings>(
        "{\"width\":" + width +
        ",\"height\":1080,\"refresh_rate\":60,\"wayland_render_node\":\"\",\"runner_render_node\":\"\","
        "\"video_producer_buffer_caps\":\"\"}");
  };

  REQUIRE(read_video("1920").value().width == 1920);
  REQUIRE(read_video("-1").value().width == -1);
  REQUIRE(read_video("2147483647").value().width == 2147483647);
  REQUIRE(read_video("-2147483648").value().width == -2147483648LL);
  REQUIRE(!read_video("2147483648"));
  REQUIRE(!read_video("4294967297")); // used to wrap to 1
  REQUIRE(!read_video("-2147483649"));
  REQUIRE(!read_video("18446744073709551615")); // used to wrap to -1
  REQUIRE(!read_video("1.5"));

  auto pin = rfl::json::read<events::StopLobbyEvent>("{\"lobby_id\":\"a\",\"pin\":[1,65537]}");
  REQUIRE(!pin);
  REQUIRE(rfl::json::read<events::StopLobbyEvent>("{\"lobby_id\":\"a\",\"pin\":[1,2]}").value().pin ==
          std::vector<short>{1, 2});

  REQUIRE_THAT(rfl::json::write(events::AudioSettings{.channel_count = 6}), Equals("{\"channel_count\":6}"));
}

// TEST_CASE("Serialize to msgpack", "[serialization]") {
//   SECTION("example from the README") {
//
//     struct Person {
//       std::string first_name;
//       std::string last_name;
//       int age;
//     };
//
//     const auto homer = Person{.first_name = "Homer", .last_name = "Simpson", .age = 45};
//
//     auto result = rfl::msgpack::read<Person>(rfl::msgpack::write(homer));
//
//     REQUIRE_THAT(result.value().first_name, Equals("Homer"));
//     REQUIRE_THAT(result.value().last_name, Equals("Simpson"));
//     REQUIRE(result.value().age == 45);
//   }
// }