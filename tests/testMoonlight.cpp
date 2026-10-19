#include <crypto/src/sign.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_container_properties.hpp>
#include <catch2/matchers/catch_matchers_contains.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <rest/endpoints.hpp>

using Catch::Matchers::Equals;

#include <boost/asio/local/stream_protocol.hpp>
#include <client_http.hpp>
#include <client_https.hpp>
#include <crypto/crypto.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <moonlight/protocol.hpp>
#include <range/v3/view.hpp>
#include <rest/helpers.hpp>
#include <rfl/toml.hpp>
#include <sessions/common.hpp>
#include <state/config.hpp>
#include <streaming/streaming.hpp>
#include <sys/stat.h>
#include <thread>

using namespace moonlight;
using namespace state;
using namespace ranges;

TEST_CASE("LocalState load TOML", "[LocalState]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto state = state::load_or_default("config.test.toml", event_bus, running_sessions);
  REQUIRE(state.hostname == "Wolf");
  REQUIRE(state.uuid == "0000-1111-2222-3333");
  REQUIRE(state.support_hevc);

  SECTION("Apps") {
    auto moonlight_profile = state::get_moonlight_profile(state);
    REQUIRE(moonlight_profile);
    immer::vector<immer::box<events::App>> apps = moonlight_profile.value()->apps->load();
    REQUIRE_THAT(apps, Catch::Matchers::SizeIs(2));

    auto first_app = apps.at(0);
    REQUIRE_THAT(first_app->base.title, Equals("Firefox"));
    REQUIRE_THAT(first_app->base.id, Equals("304556286"));
    REQUIRE_THAT(first_app->base.icon_png_path.value(), Equals("firefox.png"));
    auto default_video_source = "interpipesrc name=interpipesrc_{}_video";
    REQUIRE_THAT(first_app->h264_gst_pipeline,
                 Equals(fmt::format("{} !\ndefault !\nh264_pipeline !\nvideo_sink", default_video_source)));
    REQUIRE_THAT(first_app->hevc_gst_pipeline,
                 Equals(fmt::format("{} !\ndefault !\nhevc_pipeline !\nvideo_sink", default_video_source)));
    REQUIRE_THAT(first_app->av1_gst_pipeline,
                 Equals(fmt::format("{} !\nparams !\nav1_pipeline !\nvideo_sink", default_video_source)));
    REQUIRE(first_app->start_virtual_compositor);
    REQUIRE(first_app->render_node == "/dev/dri/renderD128");
    auto first_app_runner = rfl::get<AppDocker>(first_app->runner->serialize().variant());
    REQUIRE_THAT(first_app_runner.image, Equals("ghcr.io/games-on-whales/firefox:master"));

    auto second_app = apps.at(1);
    REQUIRE_THAT(second_app->base.title, Equals("Test ball"));
    REQUIRE_THAT(second_app->base.id, Equals("378473508"));
    REQUIRE(second_app->base.icon_png_path.has_value() == false);
    REQUIRE_THAT(second_app->h264_gst_pipeline,
                 Equals("override DEFAULT SOURCE !\ndefault !\nh264_pipeline !\nvideo_sink"));
    REQUIRE_THAT(second_app->hevc_gst_pipeline,
                 Equals("override DEFAULT SOURCE !\ndefault !\nhevc_pipeline !\nvideo_sink"));
    REQUIRE_THAT(second_app->av1_gst_pipeline,
                 Equals("override DEFAULT SOURCE !\nparams !\nav1_pipeline !\nvideo_sink"));
    REQUIRE(!second_app->start_virtual_compositor);
    REQUIRE(second_app->render_node == "/tmp/dead_beef");
    auto second_app_runner = rfl::get<AppCMD>(second_app->runner->serialize().variant());
    REQUIRE_THAT(second_app_runner.run_cmd, Equals("destroy_computer_now"));
  }

  SECTION("Paired Clients") {
    REQUIRE_THAT(state.paired_clients->load().get(), Catch::Matchers::SizeIs(1));
    REQUIRE_THAT(state.paired_clients->load().get()[0]->client_cert, Equals("A VERY VALID CERTIFICATE"));
    REQUIRE(state.paired_clients->load().get()[0]->settings.run_uid == 1234);
    REQUIRE(state.paired_clients->load().get()[0]->settings.run_gid == 5678);
    REQUIRE(state.paired_clients->load().get()[0]->settings.controllers_override[0] ==
            wolf::config::ControllerType::PS);
    REQUIRE(state.paired_clients->load().get()[0]->settings.mouse_acceleration == 2.5f);
    REQUIRE(state.paired_clients->load().get()[0]->settings.v_scroll_acceleration == 1.5f);
    REQUIRE(state.paired_clients->load().get()[0]->settings.h_scroll_acceleration == 10.2f);
    REQUIRE_THAT(state.paired_clients->load().get()[0]->app_state_folder, Equals("some/folder"));
  }
}

TEST_CASE("LocalState pairing information", "[LocalState]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto clients_atom = std::make_shared<immer::atom<state::PairedClientList>>();
  auto cfg = state::Config{.config_source = "config.test.toml", .paired_clients = clients_atom};
  auto a_client_cert = "-----BEGIN CERTIFICATE-----\n"
                       "MIICvzCCAaegAwIBAgIBADANBgkqhkiG9w0BAQsFADAjMSEwHwYDVQQDDBhOVklE\n"
                       "SUEgR2FtZVN0cmVhbSBDbGllbnQwHhcNMjEwNzEwMDgzNjE3WhcNNDEwNzA1MDgz\n"
                       "NjE3WjAjMSEwHwYDVQQDDBhOVklESUEgR2FtZVN0cmVhbSBDbGllbnQwggEiMA0G\n"
                       "CSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC1c49oJxPkIGjUNSwEpNuvNZB/hRoe\n"
                       "DZPMe3gZ1abOSJFyK3VbQNSOVK6qEfBrHqQgNVMtXZVS5XDfDRW9E37QcAl5Apex\n"
                       "/NAG2fNHYAvO6P6Gj+/m0XBWcVREzjGCdKJI+9ToNeB6dQtKw26LvlIbHfprBZ+1\n"
                       "QPr3DuUuTCIdxacJrSoUyqakswGr82j3+Dt3dU0LWCquVeeOMhMVN+EFzjFbDJbz\n"
                       "+sNLTM2E7HeKNeIr/0oFhyA2b5Kb4vd+z1GfW38o85gDU8ni6FOmEaFTi3TdZ1yR\n"
                       "RxdKKd9k+myCNHLiyBA9K4Q2JYuUkYxZzsZrKVH7LWfH79qaMvj87iEnAgMBAAEw\n"
                       "DQYJKoZIhvcNAQELBQADggEBADVm005jqiFEGuH6JfTKRcZU3PMmFrEmJTmvPiyh\n"
                       "a4b75jai0hLRCzNDu6bSzlXH346YK/x7AVkSZIoD2hTpujVlHIHLoSJzkrmMxIFR\n"
                       "sZRjh0EGaAmvDuEXADUE0MGpoWLUQumiDWwuSbEzPyr7BID68A3Q6jKwd65D2+ut\n"
                       "snLzlErTl5fgHkLWV6plelcMgPSo4T+E7APm4LHlP5uxiixAfnhDlzuAeD5r1rsD\n"
                       "UkrNfMfXnRzPSwXiNYHZ+UoQuchoMCSAa+kcsQ+zsdPwAJ3stwQGcfpvYhdZv1a1\n"
                       "oPJpyCigkmv0uH4CeJ09A/6Da2uY+HIwVq85qteMVSUTtV0=\n"
                       "-----END CERTIFICATE-----\n";

  SECTION("Checking pairing mechanism") {
    REQUIRE(state::get_client_via_ssl(cfg, a_client_cert).has_value() == false);
    state::pair(cfg, {a_client_cert});
    REQUIRE(state::get_client_via_ssl(cfg, a_client_cert).has_value() == true);

    auto another_cert = "-----BEGIN CERTIFICATE-----\n"
                        "MIIC6zCCAdOgAwIBAgIBATANBgkqhkiG9w0BAQsFADA5MQswCQYDVQQGEwJJVDEW\n"
                        "MBQGA1UECgwNR2FtZXNPbldoYWxlczESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTIy\n"
                        "MDgzMTA1NDIzM1oXDTQyMDgyNjA1NDIzM1owOTELMAkGA1UEBhMCSVQxFjAUBgNV\n"
                        "BAoMDUdhbWVzT25XaGFsZXMxEjAQBgNVBAMMCWxvY2FsaG9zdDCCASIwDQYJKoZI\n"
                        "hvcNAQEBBQADggEPADCCAQoCggEBAKYAuR+nnhekgyOvEKPhNdGiGECe5iiPo/4g\n"
                        "yzrXEIKggr1oTauqmBPUhVT6sGuuKE5cR0u4pHm7bcUZAeBdssPzq8IJBKRzEgeM\n"
                        "AtoWu+MsWijbq2wSKJTLe9e0/WClk0iFopvVrhyai2hbxqlWxyTyxWVxKg4iECAg\n"
                        "7TT9EAAuZzS5EhfOBA6+xREdDse5TzkTLWHVSNIaDgPuJiM3DXHjt64C0ubTNklO\n"
                        "VDzU5nBYwOidi64EsznpnHT7Zhj0fhB8g7rV6n4ZEi65o6vhAbC6J4IBCBXLw9R/\n"
                        "s8cglKED3ebS9gGE9V+0exXkCr+X6JqQ8tnuJaWoeXIWTctatB0CAwEAATANBgkq\n"
                        "hkiG9w0BAQsFAAOCAQEAnQ5+pnjzAylkoA/vi6DxxwlCxWsDS6HlPptlpY+AlY6q\n"
                        "PvQMHxrRMGXYWl+70ja8GvvlfZLf0zj/wc8vjxFB/+P3mOLv00YBGdryIF5Fijg9\n"
                        "h9LZ9+raPvR/xIDevubveQgX/Kcl2cX59/aaIwKPUJH4BHzRTJ2W32B8Fw1KO/sp\n"
                        "CH7xDcplhPQ6YvTedFBBu1AcSiRpmGcOuo1+HMJZLj3ek7NrFOSjfptIStleYixj\n"
                        "Gxy5pgOEuXuSoi3dJInrwc3dBILZBDYyBg7jPBDjRC+ld24q6/TYHBPKbmaM2iFa\n"
                        "AXQMq/fmtPxEDHY0NguWUhGt3uooTiakv1u9zex/Cg==\n"
                        "-----END CERTIFICATE-----";

    REQUIRE(state::get_client_via_ssl(cfg, another_cert).has_value() == false);
    state::pair(cfg, {another_cert});
    REQUIRE(state::get_client_via_ssl(cfg, another_cert).has_value() == true);
    REQUIRE_THAT(cfg.paired_clients->load().get(), Catch::Matchers::SizeIs(2));

    state::unpair(cfg, {a_client_cert});
    REQUIRE(state::get_client_via_ssl(cfg, a_client_cert).has_value() == false);
    REQUIRE(state::get_client_via_ssl(cfg, another_cert).has_value() == true);
    REQUIRE_THAT(cfg.paired_clients->load().get(), Catch::Matchers::SizeIs(1));

    state::unpair(cfg, {another_cert});
    REQUIRE_THAT(cfg.paired_clients->load().get(), Catch::Matchers::SizeIs(0));
  }
}

TEST_CASE("LocalState concurrent updates", "[LocalState]") {
  // The API applies config updates from a thread pool; none of them may be lost or tear the file (XERK-1962)
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  const std::string cfg_file = "config.test.CONCURRENT.toml";
  std::filesystem::copy_file("config.test.toml", cfg_file, std::filesystem::copy_options::overwrite_existing);
  std::filesystem::permissions(cfg_file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  auto cfg = state::load_or_default(cfg_file, event_bus, running_sessions);
  auto initial_profiles = cfg.profiles->load()->size();
  auto initial_clients = cfg.paired_clients->load()->size();

  constexpr int n_threads = 16;
  std::vector<std::thread> threads;
  for (int i = 0; i < n_threads; i++) {
    threads.emplace_back([&cfg, i]() {
      state::update_profiles(cfg, [i](const state::ProfilesList &profiles) {
        return profiles.push_back(
            events::Profile{.id = "concurrent-" + std::to_string(i),
                            .name = "Concurrent " + std::to_string(i),
                            .apps = std::make_shared<immer::atom<immer::vector<immer::box<events::App>>>>()});
      });
      state::pair(cfg, {.client_cert = "concurrent-cert-" + std::to_string(i)});
    });
  }
  // Partial settings updates to the same client, each setting a different field: all of them must stick
  auto client_id = state::get_client_id(*cfg.paired_clients->load()->front());
  auto settings_update = [&cfg, client_id](auto &&set_field) {
    return std::thread([&cfg, client_id, set_field]() {
      state::update_client_settings(cfg, client_id, [&set_field](const state::PairedClient &client) {
        auto updated = client;
        set_field(updated.settings);
        return updated;
      });
    });
  };
  threads.push_back(settings_update([](auto &s) { s.run_uid = 4242; }));
  threads.push_back(settings_update([](auto &s) { s.run_gid = 4343; }));
  threads.push_back(settings_update([](auto &s) { s.mouse_acceleration = 4.25f; }));
  threads.push_back(settings_update([](auto &s) { s.v_scroll_acceleration = 4.5f; }));
  for (auto &t : threads) {
    t.join();
  }

  REQUIRE(cfg.profiles->load()->size() == initial_profiles + n_threads);
  REQUIRE(cfg.paired_clients->load()->size() == initial_clients + n_threads);

  auto reloaded = state::load_or_default(cfg_file, event_bus, running_sessions);
  REQUIRE(reloaded.profiles->load()->size() == initial_profiles + n_threads);
  REQUIRE(reloaded.paired_clients->load()->size() == initial_clients + n_threads);
  auto reloaded_clients = reloaded.paired_clients->load();
  for (int i = 0; i < n_threads; i++) {
    auto cert = "concurrent-cert-" + std::to_string(i);
    REQUIRE(std::any_of(reloaded_clients->begin(), reloaded_clients->end(), [&cert](const auto &client) {
      return client->client_cert == cert;
    }));
  }

  auto check_settings = [client_id](const state::Config &c) {
    auto clients = c.paired_clients->load();
    auto client = std::find_if(clients->begin(), clients->end(), [client_id](const auto &cl) {
      return state::get_client_id(*cl) == client_id;
    });
    REQUIRE(client != clients->end());
    REQUIRE((*client)->settings.run_uid == 4242);
    REQUIRE((*client)->settings.run_gid == 4343);
    REQUIRE((*client)->settings.mouse_acceleration == 4.25f);
    REQUIRE((*client)->settings.v_scroll_acceleration == 4.5f);
  };
  check_settings(cfg);
  check_settings(reloaded);

  // Replacing the file must keep its permissions and leave no temp file behind
  REQUIRE((std::filesystem::status(cfg_file).permissions() & std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
  REQUIRE(!std::filesystem::exists(cfg_file + ".tmp"));

  // A symlink or FIFO planted at the temp path must be neither followed nor blocked on
  const std::string victim = "config.test.VICTIM";
  { std::ofstream(victim) << "untouched"; }
  std::filesystem::create_symlink(victim, cfg_file + ".tmp");
  state::pair(cfg, {.client_cert = "planted-symlink"});
  REQUIRE(std::filesystem::is_regular_file(std::filesystem::symlink_status(cfg_file)));
  REQUIRE(std::filesystem::file_size(victim) == std::string("untouched").size());
  std::filesystem::remove(victim);

  REQUIRE(::mkfifo((cfg_file + ".tmp").c_str(), 0600) == 0);
  state::pair(cfg, {.client_cert = "planted-fifo"});
  REQUIRE(state::load_or_default(cfg_file, event_bus, running_sessions).paired_clients->load()->size() ==
          initial_clients + n_threads + 2);
  REQUIRE(!std::filesystem::exists(std::filesystem::symlink_status(cfg_file + ".tmp")));
  std::filesystem::remove(cfg_file);
}

TEST_CASE("Default config: every encoder has video_params", "[LocalState]") {
  constexpr char const *default_toml =
#include <state/default/config.include.toml>
      ;
  auto cfg = rfl::toml::read<wolf::config::WolfConfig, rfl::DefaultIfMissing>(default_toml).value();
  auto video = cfg.gstreamer.video;
  for (const auto &encoders : {video.h264_encoders, video.hevc_encoders, video.av1_encoders}) {
    for (const auto &encoder : encoders) {
      INFO("plugin_name: " << encoder.plugin_name << ", encoder: " << encoder.check_elements.at(0));
      auto plugin_default = video.defaults.find(encoder.plugin_name);
      auto has_default = plugin_default != video.defaults.end();
      // An encoder without its own params falls back to the plugin default, otherwise to an empty string
      REQUIRE(!encoder.video_params.value_or(has_default ? plugin_default->second.video_params : "").empty());
      REQUIRE(!encoder.video_params_zero_copy.value_or(has_default ? plugin_default->second.video_params_zero_copy : "")
                   .empty());
    }
  }
}

TEST_CASE("/resume keeps what still runs for the stream", "[Sessions]") {
  using namespace wolf::core;
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto state_dir = std::filesystem::temp_directory_path() / fmt::format("wolf-resume-{}", getpid());
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions)),
      .host = immer::box<state::Host>(
          state::Host{.host_base_state_folder = state_dir.string(), .local_base_state_folder = state_dir.string()}),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto client = state::PairedClient{.client_cert = "resume-test-cert", .app_state_folder = "client"};
  auto app = events::App{.base = {.title = "app", .id = "app"}};
  auto old_session = *state::create_stream_session(app_state,
                                                   app,
                                                   client,
                                                   {1920, 1080, 60},
                                                   2,
                                                   "00112233445566778899aabbccddeeff",
                                                   "1");
  auto sink = std::make_shared<audio::VSink>();
  old_session.audio_sink->store(sink);
  running_sessions->store(immer::vector<events::StreamSession>{old_session});

  SimpleWeb::CaseInsensitiveMultimap query = {{"mode", "1280x720x30"},
                                              {"rikey", "ffeeddccbbaa99887766554433221100"},
                                              {"rikeyid", "2"}};
  auto resumed = endpoints::https::resume_session(query, "127.0.0.1", client, app_state);
  REQUIRE(resumed);
  REQUIRE(running_sessions->load()->size() == 1);
  auto running = running_sessions->load()->front();
  // A new instance of the same client's stream...
  REQUIRE(running.session_id == old_session.session_id);
  REQUIRE(running.enet_secret_payload == resumed->enet_secret_payload);
  REQUIRE(running.display_mode.width == 1280);
  // ...that keeps the desktop, its producers and its audio sink: the sink is removed when the session stops, and a
  // resumed session that lost it would leave it behind
  REQUIRE(running.wayland_display == old_session.wayland_display);
  REQUIRE(running.producer_name == old_session.producer_name);
  REQUIRE(running.audio_sink->load() == sink);

  std::filesystem::remove_all(state_dir);
}

TEST_CASE("PulseAudio server name to socket path", "[Sessions]") {
  using wolf::core::sessions::audio_server_socket_path;
  REQUIRE(audio_server_socket_path("/tmp/sockets/pulse-socket") == "/tmp/sockets/pulse-socket");
  // Docker splits binds on ':', so the prefix must not reach the mount
  REQUIRE(audio_server_socket_path("unix:/run/pulse/native") == "/run/pulse/native");
  REQUIRE(audio_server_socket_path("{0123abcd}unix:/run/pulse/native") == "/run/pulse/native");
  REQUIRE(audio_server_socket_path("tcp:localhost:4713").empty());
  REQUIRE(audio_server_socket_path("").empty());
}

TEST_CASE("Wayland socket wait", "[Sessions]") {
  // CTest sets WOLF_SKIP_WAYLAND_SOCKET_WAIT for the whole suite; this test needs the real wait
  std::optional<std::string> skip_wait;
  if (auto env = std::getenv("WOLF_SKIP_WAYLAND_SOCKET_WAIT")) {
    skip_wait = env;
  }
  ::unsetenv("WOLF_SKIP_WAYLAND_SOCKET_WAIT");
  auto runtime_dir = std::filesystem::temp_directory_path().string();

  // No virtual compositor (e.g. "Test ball"): no socket name, must not wait for the runtime dir to become one
  REQUIRE(wolf::core::sessions::wait_for_wayland_socket(runtime_dir, "", false, std::chrono::milliseconds(100)));
  // A compositor that reports no socket can't be reached by the runner, whether or not the wait is skipped (without
  // the skip, runtime_dir/"" is a directory, so the wait would also fail by timing out)
  ::setenv("WOLF_SKIP_WAYLAND_SOCKET_WAIT", "TRUE", 1);
  REQUIRE_FALSE(wolf::core::sessions::wait_for_wayland_socket(runtime_dir, "", true, std::chrono::milliseconds(100)));
  ::unsetenv("WOLF_SKIP_WAYLAND_SOCKET_WAIT");
  REQUIRE_FALSE(wolf::core::sessions::wait_for_wayland_socket(runtime_dir, "", true, std::chrono::milliseconds(100)));
  REQUIRE_FALSE(wolf::core::sessions::wait_for_wayland_socket(runtime_dir,
                                                              "wolf-no-such-socket",
                                                              true,
                                                              std::chrono::milliseconds(100)));

  // A timeout of 0 still finds a socket that's already there (it used to always fail)
  auto socket_name = fmt::format("wolf-test-socket-{}", getpid());
  auto socket_path = std::filesystem::path(runtime_dir) / socket_name;
  std::filesystem::remove(socket_path);
  {
    boost::asio::io_context io;
    boost::asio::local::stream_protocol::acceptor listener(io, socket_path.string());
    REQUIRE(
        wolf::core::sessions::wait_for_wayland_socket(runtime_dir, socket_name, true, std::chrono::milliseconds(0)));
  }
  std::filesystem::remove(socket_path);

  if (skip_wait) {
    ::setenv("WOLF_SKIP_WAYLAND_SOCKET_WAIT", skip_wait->c_str(), 1);
  }
}

TEST_CASE("LocalState reports a config file it couldn't save", "[LocalState]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  const std::string cfg_file = "config.test.EMPTIED.toml";
  std::filesystem::copy_file("config.test.toml", cfg_file, std::filesystem::copy_options::overwrite_existing);
  auto cfg = state::load_or_default(cfg_file, event_bus, running_sessions);
  auto client_id = state::get_client_id(*cfg.paired_clients->load()->front());
  auto keep_client = [](const state::PairedClient &client) { return client; };
  auto keep_profiles = [](const state::ProfilesList &profiles) { return profiles; };

  REQUIRE(state::pair(cfg, {.client_cert = "saved-cert"}));
  REQUIRE(state::update_client_settings(cfg, client_id, keep_client) == state::ConfigUpdate::SAVED);
  REQUIRE(state::update_client_settings(cfg, 42, keep_client) == state::ConfigUpdate::NOT_FOUND);
  REQUIRE(state::update_profiles(cfg, keep_profiles));

  // Emptied under us: it loads as all defaults, and saving that back would write an empty uuid and hostname and
  // keep only the client being paired
  { std::ofstream(cfg_file, std::ios::trunc); }
  REQUIRE_FALSE(state::pair(cfg, {.client_cert = "unsaved-cert"}));
  REQUIRE_FALSE(state::unpair(cfg, {.client_cert = "saved-cert"}));
  REQUIRE(state::update_client_settings(cfg, client_id, keep_client) == state::ConfigUpdate::NOT_SAVED);
  REQUIRE_FALSE(state::update_profiles(cfg, keep_profiles));
  REQUIRE(std::filesystem::file_size(cfg_file) == 0);
  std::filesystem::remove(cfg_file);
}

TEST_CASE("LocalState: blank producer caps fall back to the default", "[LocalState]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto cfg = state::load_or_default("config.test.toml", event_bus, running_sessions);
  auto app = wolf::config::BaseApp{.title = "caps", .runner = wolf::config::AppCMD{.run_cmd = "true"}};
  auto default_caps = cfg.parse_app(app).video_producer_buffer_caps;
  REQUIRE_FALSE(default_caps.empty());
  for (auto caps : {"", " ", "\t\n "}) {
    app.video = wolf::config::BaseAppVideoOverride{.producer_buffer_caps = caps};
    REQUIRE_THAT(cfg.parse_app(app).video_producer_buffer_caps, Equals(default_caps));
  }
  app.video = wolf::config::BaseAppVideoOverride{.producer_buffer_caps = " video/x-raw(memory:DMABuf)"};
  REQUIRE_THAT(cfg.parse_app(app).video_producer_buffer_caps, Equals(" video/x-raw(memory:DMABuf)"));
}

TEST_CASE("Encoder video_params fall back to the embedded default", "[LocalState]") {
  constexpr char const *default_toml =
#include <state/default/config.include.toml>
      ;
  auto embedded =
      rfl::toml::read<wolf::config::WolfConfig, rfl::DefaultIfMissing>(default_toml).value().gstreamer.video;
  auto params = [&](const GstEncoder &encoder, const GstVideoCfg &user_cfg, bool zero_copy) {
    return state::encoder_video_params("h264", encoder, user_cfg, embedded, zero_copy);
  };
  auto embedded_params = [](const std::vector<GstEncoder> &encoders, std::string_view plugin_name) {
    auto found =
        std::find_if(encoders.begin(), encoders.end(), [&](const auto &e) { return e.plugin_name == plugin_name; });
    REQUIRE(found != encoders.end());
    return found->video_params.value();
  };

  // A config.toml generated before XERK-2026: x264 without params and no defaults.x264
  auto old_x264 = GstEncoder{.plugin_name = "x264", .check_elements = {"x264enc"}, .encoder_pipeline = "x264enc"};
  auto embedded_x264 = std::find_if(embedded.h264_encoders.begin(), embedded.h264_encoders.end(), [](const auto &e) {
    return e.plugin_name == "x264";
  });
  REQUIRE(embedded_x264 != embedded.h264_encoders.end());
  REQUIRE_THAT(params(old_x264, GstVideoCfg{}, false), Equals(embedded_x264->video_params.value()));
  REQUIRE_THAT(params(old_x264, GstVideoCfg{}, true), Equals(embedded_x264->video_params_zero_copy.value()));

  // The fallback also covers plugins whose params live in the embedded [gstreamer.video.defaults]
  auto old_va = GstEncoder{.plugin_name = "va", .check_elements = {"vah264enc"}, .encoder_pipeline = "vah264enc"};
  REQUIRE_THAT(params(old_va, GstVideoCfg{}, true), Equals(embedded.defaults.at("va").video_params_zero_copy));

  // The user's own params always win, inline or via their defaults
  auto custom = old_x264;
  custom.video_params = "custom";
  REQUIRE_THAT(params(custom, GstVideoCfg{}, false), Equals("custom"));
  auto user_defaults = GstVideoCfg{.defaults = {{"x264", {.video_params = "mine", .video_params_zero_copy = "mine0"}}}};
  REQUIRE_THAT(params(old_x264, user_defaults, true), Equals("mine0"));

  // Each codec falls back to its own list: x265 only exists for HEVC, aom only for AV1
  auto old_x265 = GstEncoder{.plugin_name = "x265", .check_elements = {"x265enc"}};
  auto old_aom = GstEncoder{.plugin_name = "aom", .check_elements = {"av1enc"}};
  REQUIRE_THAT(state::encoder_video_params("h265", old_x265, GstVideoCfg{}, embedded, false),
               Equals(embedded_params(embedded.hevc_encoders, "x265")));
  REQUIRE_THAT(state::encoder_video_params("av1", old_aom, GstVideoCfg{}, embedded, false),
               Equals(embedded_params(embedded.av1_encoders, "aom")));
  REQUIRE(state::encoder_video_params("h264", old_x265, GstVideoCfg{}, embedded, false).empty());

  // A plugin the embedded config doesn't know stays empty
  auto unknown = GstEncoder{.plugin_name = "coreelements", .check_elements = {"identity"}};
  REQUIRE(params(unknown, GstVideoCfg{}, false).empty());
}

TEST_CASE("Mocked serverinfo", "[MoonlightProtocol]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto cfg = state::load_or_default("config.test.toml", event_bus, running_sessions);
  immer::array<DisplayMode> displayModes = {{1920, 1080, 60}, {1024, 768, 30}};

  SECTION("server_info conforms with the expected HEVC response") {
    auto result = serverinfo(false,
                             0,
                             0,
                             1,
                             cfg.uuid,
                             cfg.hostname,
                             "AA:BB:CC:DD",
                             "192.168.99.1",
                             displayModes,
                             false,
                             true,
                             false);

    REQUIRE(xml_to_str(result) ==
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<root status_code=\"200\">"
            "<hostname>Wolf</hostname>"
            "<appversion>7.1.431.-1</appversion>"
            "<GfeVersion>3.23.0.74</GfeVersion>"
            "<uniqueid>0000-1111-2222-3333</uniqueid>"
            "<MaxLumaPixelsHEVC>1869449984</MaxLumaPixelsHEVC>"
            "<ServerCodecModeSupport>257</ServerCodecModeSupport>"
            "<HttpsPort>0</HttpsPort>"
            "<ExternalPort>1</ExternalPort>"
            "<mac>AA:BB:CC:DD</mac>"
            "<LocalIP>192.168.99.1</LocalIP>"
            "<SupportedDisplayMode>"
            "<DisplayMode><Width>1920</Width><Height>1080</Height><RefreshRate>60</RefreshRate></DisplayMode>"
            "<DisplayMode><Width>1024</Width><Height>768</Height><RefreshRate>30</RefreshRate></DisplayMode>"
            "</SupportedDisplayMode><PairStatus>0</PairStatus>"
            "<currentgame>0</currentgame>"
            "<state>SUNSHINE_SERVER_FREE</state>"
            "</root>");
    REQUIRE(result.get<bool>("root.PairStatus") == false);
  }

  SECTION("server_info conforms with the expected AV1 response") {
    auto result = serverinfo(true,
                             0,
                             0,
                             1,
                             cfg.uuid,
                             cfg.hostname,
                             "AA:BB:CC:DD",
                             "192.168.1.1",
                             displayModes,
                             true,
                             false,
                             true);

    REQUIRE(xml_to_str(result) ==
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<root status_code=\"200\">"
            "<hostname>Wolf</hostname>"
            "<appversion>7.1.431.-1</appversion>"
            "<GfeVersion>3.23.0.74</GfeVersion>"
            "<uniqueid>0000-1111-2222-3333</uniqueid>"
            "<MaxLumaPixelsHEVC>0</MaxLumaPixelsHEVC>"
            "<ServerCodecModeSupport>65537</ServerCodecModeSupport>"
            "<HttpsPort>0</HttpsPort>"
            "<ExternalPort>1</ExternalPort>"
            "<mac>AA:BB:CC:DD</mac>"
            "<LocalIP>192.168.1.1</LocalIP>"
            "<SupportedDisplayMode>"
            "<DisplayMode><Width>1920</Width><Height>1080</Height><RefreshRate>60</RefreshRate></DisplayMode>"
            "<DisplayMode><Width>1024</Width><Height>768</Height><RefreshRate>30</RefreshRate></DisplayMode>"
            "</SupportedDisplayMode>"
            "<PairStatus>1</PairStatus>"
            "<currentgame>0</currentgame>"
            "<state>SUNSHINE_SERVER_BUSY</state>"
            "</root>");
    REQUIRE(result.get<bool>("root.PairStatus") == true);
  }
}

// Stuff generated from Moonlight
struct PairingData {
  std::string pin = "7284";
  std::string salt = "a0c288cfb0ea624ec3e5cc54d6ab7e38";
  std::string client_challenge = "60418ac415307d7a1f9695ba04e1ae34";
  std::string server_challenge_resp = "2798e4a56fa102558e8c93b892fbbe17b06b0cac91855ceb536ca2def53f7cf1";
  std::string client_pairing_secret =
      "3dda32b87413e98005d608df9c54323f507ccaef60a5a31acfc2b9bc64bb27e3cc467555bf81bc0464dc4b1488810d435266f31e22acd275"
      "07bce4a8173d1b4c5ea51f47d702a074e9cf95c6ff6cb7cf85e1abf9d236c5e3b638601092adc6a993f77b0585100994cecec88e01023f7f"
      "0ef13e8d66f5ba89608f1838c85d214b91a4be276d9ad555382a599b142b1e62b571427dc30697342da9995bfc5362cc27f09a3cb3622eda"
      "9f6ccad3385e296e5b9b296377523e0e41b5748d60ea96f2ebabb9c30c13e98cef420d87e266f72bfc2e18cf584a03e6141e9dd4967dd77e"
      "4ce99b20f73e37caf801931babbc4ff75df4013b529651b97d7bfc228d2c13785c50e22071bc6ce1332ee777a009c630";
  std::string client_cert_base64 =
      "2d2d2d2d2d424547494e2043455254494649434154452d2d2d2d2d0a4d494943767a43434161656741774942416749424144414e42676b71"
      "686b694739773042415173464144416a4d53457748775944565151444442684f566b6c450a53554567523246745a564e30636d5668625342"
      "4462476c6c626e51774868634e4d6a45774e7a45774d44677a4e6a45335768634e4e4445774e7a41314d44677a0a4e6a4533576a416a4d53"
      "457748775944565151444442684f566b6c4553554567523246745a564e30636d56686253424462476c6c626e5177676745694d4130470a43"
      "53714753496233445145424151554141344942447741776767454b416f4942415143316334396f4a78506b49476a554e537745704e75764e"
      "5a422f68526f650a445a504d6533675a3161624f534a46794b335662514e534f564b367145664272487151674e564d74585a565335584466"
      "445257394533375163416c35417065780a2f4e414732664e485941764f365036476a2b2f6d30584257635652457a6a4743644b4a492b3954"
      "6f4e6542366451744b7732364c766c496248667072425a2b310a5150723344755575544349647861634a72536f557971616b737747723832"
      "6a332b4474336455304c574371755665654f4d684d564e2b45467a6a4662444a627a0a2b734e4c544d32453748654b4e6549722f306f4668"
      "79413262354b623476642b7a3147665733386f3835674455386e6936464f6d45614654693354645a3179520a5278644b4b64396b2b6d7943"
      "4e484c69794241394b3451324a5975556b59785a7a735a724b5648374c576648373971614d766a383769456e41674d42414145770a445159"
      "4a4b6f5a496876634e4151454c42514144676745424144566d3030356a71694645477548364a66544b52635a5533504d6d4672456d4a546d"
      "76506979680a61346237356a616930684c52437a4e44753662537a6c5848333436594b2f783741566b535a496f4432685470756a566c4849"
      "484c6f534a7a6b726d4d784946520a735a526a6830454761416d764475455841445545304d47706f574c5551756d69445777755362457a50"
      "7972374249443638413351366a4b7764363544322b75740a736e4c7a6c4572546c356667486b4c575636706c656c634d6750536f34542b45"
      "3741506d344c486c5035757869697841666e68446c7a754165443572317273440a556b724e664d66586e527a50537758694e59485a2b556f"
      "517563686f4d435341612b6b6373512b7a73645077414a337374775147636670765968645a763161310a6f504a70794369676b6d76307548"
      "3443654a3039412f3644613275592b484977567138357174654d565355547456303d0a2d2d2d2d2d454e442043455254494649434154452d"
      "2d2d2d2d0a";
};

// Stuff generated on our side
struct ServerData {
  std::string server_secret = crypto::hex_to_str("AB7D178785415C893A6757C1B736105A", true);
  x509::x509_ptr server_cert =
      x509::cert_from_string("-----BEGIN CERTIFICATE-----\n"
                             "MIIC6zCCAdOgAwIBAgIBATANBgkqhkiG9w0BAQsFADA5MQswCQYDVQQGEwJJVDEW\n"
                             "MBQGA1UECgwNR2FtZXNPbldoYWxlczESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTIy\n"
                             "MDQwOTA5MTYwNVoXDTQyMDQwNDA5MTYwNVowOTELMAkGA1UEBhMCSVQxFjAUBgNV\n"
                             "BAoMDUdhbWVzT25XaGFsZXMxEjAQBgNVBAMMCWxvY2FsaG9zdDCCASIwDQYJKoZI\n"
                             "hvcNAQEBBQADggEPADCCAQoCggEBAMt482VY3ToUuUy6NbMhfxQgI7tJZ8fkNeVp\n"
                             "9WOnHCL9YKR07oXGLGpE0a7vXAy8lcVsOU1Hx+pfbGj56rXsne4Uqf6p2OY/cvfx\n"
                             "uSrGGgn+cKteR4bIJND4Nq6DrdlhIl5bYyZ/4sBHn+L99Zh9elKVtx/lclA8Ra8Q\n"
                             "2kupa7405TnR0lcgRVilRdHHb7HhlvCQfu1Umb3gv4I5TKIkpA/JaBTZoWzIkbAc\n"
                             "V9499JSl9gepsdlX8guljn1UlqKsHAT31vH+YG8wjtqEGYlNIO4N98lw8OEUXmRl\n"
                             "rRSRA+s++FdxBpJG2Lu/RWicRCPylNKcZiv2S1YqT3bDEPKf1LcCAwEAATANBgkq\n"
                             "hkiG9w0BAQsFAAOCAQEAqPBqzvDjl89pZMll3Ge8RS7HeDuzgocrhOcT2jnk4ag7\n"
                             "/TROZuISjDp6+SnL3gPEt7E2OcFAczTg3l/wbT5PFb6vM96saLm4EP0zmLfK1FnM\n"
                             "JDRahKutP9rx6RO5OHqsUB+b4jA4W0L9UnXUoLKbjig501AUix0p52FBxu+HJ90r\n"
                             "HlLs3Vo6nj4Z/PZXrzaz8dtQ/KJMpd/g/9xlo6BKAnRk5SI8KLhO4hW6zG0QA56j\n"
                             "X4wnh1bwdiidqpcgyuKossLOPxbS786WmsesaAWPnpoY6M8aija+ALwNNuWWmyMg\n"
                             "9SVDV76xJzM36Uq7Kg3QJYTlY04WmPIdJHkCtXWf9g==\n"
                             "-----END CERTIFICATE-----");
  std::string server_pkey = "-----BEGIN PRIVATE KEY-----\n"
                            "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDLePNlWN06FLlM\n"
                            "ujWzIX8UICO7SWfH5DXlafVjpxwi/WCkdO6FxixqRNGu71wMvJXFbDlNR8fqX2xo\n"
                            "+eq17J3uFKn+qdjmP3L38bkqxhoJ/nCrXkeGyCTQ+Daug63ZYSJeW2Mmf+LAR5/i\n"
                            "/fWYfXpSlbcf5XJQPEWvENpLqWu+NOU50dJXIEVYpUXRx2+x4ZbwkH7tVJm94L+C\n"
                            "OUyiJKQPyWgU2aFsyJGwHFfePfSUpfYHqbHZV/ILpY59VJairBwE99bx/mBvMI7a\n"
                            "hBmJTSDuDffJcPDhFF5kZa0UkQPrPvhXcQaSRti7v0VonEQj8pTSnGYr9ktWKk92\n"
                            "wxDyn9S3AgMBAAECggEAbEhQ14WELg2rUz7hpxPTaiV0fo4hEcrMN+u8sKzVF3Xa\n"
                            "QYsNCNoe9urq3/r39LtDxU3D7PGfXYYszmz50Jk8ruAGW8WN7XKkv3i/fxjv8JOc\n"
                            "6EYDMKJAnYkKqLLhCQddX/Oof2udg5BacVWPpvhX6a1NSEc2H6cDupfwZEWkVhMi\n"
                            "bCC3JcNmjFa8N7ow1/5VQiYVTjpxfV7GY1GRe7vMvBucdQKH3tUG5PYXKXytXw/j\n"
                            "KDLaECiYVT89KbApkI0zhy7I5g3LRq0Rs5fmYLCjVebbuAL1W5CJHFJeFOgMKvnO\n"
                            "QSl7MfHkTnzTzUqwkwXjgNMGsTosV4UloL9gXVF6GQKBgQD5fI771WETkpaKjWBe\n"
                            "6XUVSS98IOAPbTGpb8CIhSjzCuztNAJ+0ey1zklQHonMFbdmcWTkTJoF3ECqAos9\n"
                            "vxB4ROg+TdqGDcRrXa7Twtmhv66QvYxttkaK3CqoLX8CCTnjgXBCijo6sCpo6H1T\n"
                            "+y55bBDpxZjNFT5BV3+YPBfWQwKBgQDQyNt+saTqJqxGYV7zWQtOqKORRHAjaJpy\n"
                            "m5035pky5wORsaxQY8HxbsTIQp9jBSw3SQHLHN/NAXDl2k7VAw/axMc+lj9eW+3z\n"
                            "2Hv5LVgj37jnJYEpYwehvtR0B4jZnXLyLwShoBdRPkGlC5fs9+oWjQZoDwMLZfTg\n"
                            "eZVOJm6SfQKBgQDfxYcB/kuKIKsCLvhHaSJpKzF6JoqRi6FFlkScrsMh66TCxSmP\n"
                            "0n58O0Cqqhlyge/z5LVXyBVGOF2Pn6SAh4UgOr4MVAwyvNp2aprKuTQ2zhSnIjx4\n"
                            "k0sGdZ+VJOmMS/YuRwUHya+cwDHp0s3Gq77tja5F38PD/s/OD8sUIqJGvQKBgBfI\n"
                            "6ghy4GC0ayfRa+m5GSqq14dzDntaLU4lIDIAGS/NVYDBhunZk3yXq99Mh6/WJQVf\n"
                            "Uc77yRsnsN7ekeB+as33YONmZm2vd1oyLV1jpwjfMcdTZHV8jKAGh1l4ikSQRUoF\n"
                            "xTdMb5uXxg6xVWtvisFq63HrU+N2iAESmMnAYxRZAoGAVEFJRRjPrSIUTCCKRiTE\n"
                            "br+cHqy6S5iYRxGl9riKySBKeU16fqUACIvUqmqlx4Secj3/Hn/VzYEzkxcSPwGi\n"
                            "qMgdS0R+tacca7NopUYaaluneKYdS++DNlT/m+KVHqLynQr54z1qBlThg9KGrpmM\n"
                            "LGZkXtQpx6sX7v3Kq56PkNk=\n"
                            "-----END PRIVATE KEY-----";
};

TEST_CASE("Pairing moonlight", "[MoonlightProtocol]") {
  PairingData moonlight_data;
  ServerData server_data;

  // PHASE 1
  auto server_cert_pem = x509::get_cert_pem(server_data.server_cert);
  auto [xml_p1, aes_key] = moonlight::pair::get_server_cert(moonlight_data.pin, moonlight_data.salt, server_cert_pem);
  REQUIRE(crypto::str_to_hex(aes_key) == "8A0191F59F31950D5DE3396901AA585D");

  // PHASE 2
  auto server_cert_sig = x509::get_cert_signature(server_data.server_cert);
  auto [xml_p2, server_secret_pair] = moonlight::pair::send_server_challenge(aes_key,
                                                                             moonlight_data.client_challenge,
                                                                             server_cert_sig,
                                                                             server_data.server_secret,
                                                                             server_data.server_secret);
  auto [server_secret_ret, server_challenge_ret] = server_secret_pair;
  REQUIRE(server_data.server_secret == server_secret_ret);
  REQUIRE(server_data.server_secret == server_challenge_ret);
  REQUIRE(xml_p2.get<std::string>("root.challengeresponse") ==
          "2A86F54B35997EAE9CC30D3F06E66D93178FB7892180FEE2B4BBB8C68473BDE13401EFE6604815200DE9BEA7D2215FC4");

  // PHASE 3
  auto [xml_p3, client_hash] = moonlight::pair::get_client_hash(aes_key,
                                                                server_data.server_secret,
                                                                moonlight_data.server_challenge_resp,
                                                                server_data.server_pkey);
  REQUIRE(crypto::str_to_hex(client_hash) == "3875FFF759355205355EFE5D3065CD776A06A806CAAE0CEFAEF22475D593CEA4");
  auto pairing_secret = xml_p3.get<std::string>("root.pairingsecret");
  REQUIRE(
      pairing_secret ==
      "AB7D178785415C893A6757C1B736105A9D9B953D2D5DE21B5CA8483FFF677223A2C5E625510E16C6AA28B05E58B3EADC7D77EA32EE3684C4"
      "1C735D4F42B2FE2B5A4498988CAF01E1DFC42E801F19D8F8417CA4EFA3144E9972B948B451309F045E1C678330065D46E6C8260AD720663A"
      "FFC8DF985D8E5AB4D74CCEA266ED6A227C9F61BACF0968925A327368367EB5CF9507D21B85A3AB9F782A6DB927A013C6FD1C6201BFCE480D"
      "29BDCE706389AB4A735B5115F1E42A19FFB735617DD7EF2B46558DACDF352F33564C663E84D130F3FCBB81BC5CC125E3F772472F9A0350C1"
      "0D6B9B5ECEFE177514E8AF496E1E210FBC03571DDB222B3AA97E76BE5A7D7731756CFE3B1202237D38433F789F87B3FA");

  // PHASE 4
  auto client_cert = x509::cert_from_string(crypto::hex_to_str(moonlight_data.client_cert_base64, true));
  auto client_public_cert_signature = x509::get_cert_signature(client_cert);
  auto client_cert_public_key = x509::get_cert_public_key(client_cert);
  auto xml_p4 = moonlight::pair::client_pair(aes_key,
                                             server_challenge_ret,
                                             client_hash,
                                             moonlight_data.client_pairing_secret,
                                             client_public_cert_signature,
                                             client_cert_public_key);
  REQUIRE(xml_p4.get<int>("root.paired") == 1);
}

TEST_CASE("HTTP Pairing", "[MoonlightProtocol]") {
  PairingData moonlight_data;
  ServerData server_data;

  immer::box<state::AppState> app_state = {state::AppState{
      .config = state::Config{.config_source = "config.test.PAIRING.toml",
                              .paired_clients = std::make_shared<immer::atom<state::PairedClientList>>()},
      .host = {state::Host{.server_cert = server_data.server_cert,
                           .server_pkey = signature::create_key(server_data.server_pkey, true)}},
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = std::make_shared<events::EventBusType>()}};

  auto pair_handler = app_state->event_bus->register_handler<immer::box<events::PairSignal>>(
      [moonlight_data](const immer::box<events::PairSignal> &signal) {
        signal->user_pin->set_value(moonlight_data.pin);
      });

  auto cache_key = "1234@0.0.0.0";

  SECTION("Normal pairing flow") {
    auto phase_1 = endpoints::pair_phase1(app_state,
                                          "0.0.0.0",
                                          "1.1.1.1",
                                          moonlight_data.client_cert_base64,
                                          moonlight_data.salt,
                                          cache_key);
    // Phase 1
    auto result = phase_1->get_future().get();
    REQUIRE(result.status == SimpleWeb::StatusCode::success_ok);

    REQUIRE(app_state->pairing_cache->load().get().find(cache_key));
    auto client_cache = *(app_state->pairing_cache->load().get().find(cache_key));
    REQUIRE(crypto::str_to_hex(client_cache.aes_key) == "8A0191F59F31950D5DE3396901AA585D");

    // Phase 2
    auto phase_2 = endpoints::pair_phase2(app_state, client_cache, moonlight_data.client_challenge, cache_key);
    REQUIRE(phase_2.status == SimpleWeb::StatusCode::success_ok);
    REQUIRE(client_cache.server_secret.has_value());    // random
    REQUIRE(client_cache.server_challenge.has_value()); // random
    // Re-run so that we can make it deterministic
    {
      auto server_cert_sig = x509::get_cert_signature(server_data.server_cert);
      auto [xml_p2, server_secret_pair] = moonlight::pair::send_server_challenge(client_cache.aes_key,
                                                                                 moonlight_data.client_challenge,
                                                                                 server_cert_sig,
                                                                                 server_data.server_secret,
                                                                                 server_data.server_secret);
      auto [server_secret, server_challenge] = server_secret_pair;
      client_cache.server_secret = server_secret;
      client_cache.server_challenge = server_challenge;
    }

    // Phase 3
    auto phase_3 = endpoints::pair_phase3(app_state, client_cache, moonlight_data.server_challenge_resp, cache_key);
    REQUIRE(phase_3.status == SimpleWeb::StatusCode::success_ok);
    REQUIRE(client_cache.client_hash.has_value());
    REQUIRE(crypto::str_to_hex(client_cache.client_hash.value()) ==
            "3875FFF759355205355EFE5D3065CD776A06A806CAAE0CEFAEF22475D593CEA4");

    // Phase 4
    auto phase_4 = endpoints::pair_phase4(client_cache, moonlight_data.client_pairing_secret);
    INFO(xml_to_str(phase_4.xml));
    REQUIRE(phase_4.status == SimpleWeb::StatusCode::success_ok);
  }

  SECTION("Out of order protections") {
    PairCache client_cache = {};

    // Calling phase 2 before phase 1
    auto phase_2 = endpoints::pair_phase2(app_state, client_cache, moonlight_data.client_challenge, cache_key);
    REQUIRE(phase_2.status == SimpleWeb::StatusCode::client_error_bad_request);

    // Calling phase 1 twice
    auto phase_1 = endpoints::pair_phase1(app_state,
                                          "0.0.0.0",
                                          "1.1.1.1",
                                          moonlight_data.client_cert_base64,
                                          moonlight_data.salt,
                                          cache_key);
    // First time it works
    auto result = phase_1->get_future().get();
    REQUIRE(result.status == SimpleWeb::StatusCode::success_ok);

    client_cache = *(app_state->pairing_cache->load().get().find(cache_key)); // Copy it out for later steps

    // Second time should fail
    phase_1 = endpoints::pair_phase1(app_state,
                                     "0.0.0.0",
                                     "1.1.1.1",
                                     moonlight_data.client_cert_base64,
                                     moonlight_data.salt,
                                     cache_key);
    result = phase_1->get_future().get();
    REQUIRE(result.status == SimpleWeb::StatusCode::client_error_bad_request);
    REQUIRE(app_state->pairing_cache->load().get().find(cache_key) == nullptr); // We should have removed from the cache

    // Calling phase 3 before phase 2
    auto phase_3 = endpoints::pair_phase3(app_state, client_cache, moonlight_data.server_challenge_resp, cache_key);
    REQUIRE(phase_3.status == SimpleWeb::StatusCode::client_error_bad_request);

    // Calling phase 4 before phase 3
    auto phase_4 = endpoints::pair_phase4(client_cache, moonlight_data.client_pairing_secret);
    REQUIRE(phase_4.status == SimpleWeb::StatusCode::client_error_bad_request);
  }

  SECTION("Malformed challenges are rejected") {
    auto phase_1 = endpoints::pair_phase1(app_state,
                                          "0.0.0.0",
                                          "1.1.1.1",
                                          moonlight_data.client_cert_base64,
                                          moonlight_data.salt,
                                          cache_key);
    REQUIRE(phase_1->get_future().get().status == SimpleWeb::StatusCode::success_ok);
    auto client_cache = *(app_state->pairing_cache->load().get().find(cache_key));

    // 15 bytes: not a whole AES block, so the unpadded ECB decrypt fails
    auto bad_challenge = std::string("0102030405060708090a0b0c0d0e0f");
    auto bad_phase_2 = endpoints::pair_phase2(app_state, client_cache, bad_challenge, cache_key);
    REQUIRE(bad_phase_2.status == SimpleWeb::StatusCode::client_error_bad_request);
    REQUIRE(bad_phase_2.xml.get<int>("root.paired") == 0);

    client_cache.last_phase = state::PAIR_PHASE::GETSERVERCERT;
    REQUIRE(endpoints::pair_phase2(app_state, client_cache, moonlight_data.client_challenge, cache_key).status ==
            SimpleWeb::StatusCode::success_ok);

    auto bad_phase_3 = endpoints::pair_phase3(app_state, client_cache, bad_challenge, cache_key);
    REQUIRE(bad_phase_3.status == SimpleWeb::StatusCode::client_error_bad_request);
    REQUIRE(bad_phase_3.xml.get<int>("root.paired") == 0);
  }

  SECTION("The handler drops the pairing session after a failed phase 2/3 or phase 4") {
    // Same key as `cache_key`, built the way the /pair handler builds it
    REQUIRE(endpoints::pair_cache_key("1234", "0.0.0.0") == cache_key);
    auto bad_challenge = std::string("0102030405060708090a0b0c0d0e0f"); // 15 bytes, see above

    auto start_pairing = [&]() {
      auto phase_1 = endpoints::pair_phase1(app_state,
                                            "0.0.0.0",
                                            "1.1.1.1",
                                            moonlight_data.client_cert_base64,
                                            moonlight_data.salt,
                                            cache_key);
      REQUIRE(phase_1->get_future().get().status == SimpleWeb::StatusCode::success_ok);
    };
    auto next_phase = [&](const std::string &param, const std::string &value) {
      auto result = endpoints::pair_next_phase(app_state, {{param, value}}, "1234", "0.0.0.0");
      REQUIRE(result.has_value());
      return result.value();
    };
    auto require_session_gone = [&](const endpoints::XMLResult &retry) {
      REQUIRE(app_state->pairing_cache->load().get().find(cache_key) == nullptr);
      REQUIRE(retry.status == SimpleWeb::StatusCode::client_error_bad_request);
      REQUIRE_THAT(retry.xml.get<std::string>("root.<xmlattr>.status_message"),
                   Equals("Unable to find 1234 0.0.0.0 in the pairing cache"));
    };

    SECTION("phase 2") {
      start_pairing();
      REQUIRE(next_phase("clientchallenge", bad_challenge).status == SimpleWeb::StatusCode::client_error_bad_request);

      // Retrying with a valid challenge must not resume the rejected session
      require_session_gone(next_phase("clientchallenge", moonlight_data.client_challenge));
    }

    SECTION("phase 3") {
      start_pairing();
      REQUIRE(next_phase("clientchallenge", moonlight_data.client_challenge).status ==
              SimpleWeb::StatusCode::success_ok);
      REQUIRE(next_phase("serverchallengeresp", bad_challenge).status ==
              SimpleWeb::StatusCode::client_error_bad_request);

      require_session_gone(next_phase("serverchallengeresp", moonlight_data.server_challenge_resp));
    }

    SECTION("phase 4 pairs the client and ends the session") {
      std::filesystem::copy_file("config.test.toml",
                                 app_state->config->config_source,
                                 std::filesystem::copy_options::overwrite_existing);
      start_pairing();
      REQUIRE(next_phase("clientchallenge", moonlight_data.client_challenge).status ==
              SimpleWeb::StatusCode::success_ok);
      // Swap the random server secret/challenge for the fixed ones the test vectors were made with
      app_state->pairing_cache->update([&](const immer::map<std::string, state::PairCache> &pairing_cache) {
        auto client_cache = pairing_cache.at(cache_key);
        client_cache.server_secret = server_data.server_secret;
        client_cache.server_challenge = server_data.server_secret;
        return pairing_cache.set(cache_key, client_cache);
      });
      REQUIRE(next_phase("serverchallengeresp", moonlight_data.server_challenge_resp).status ==
              SimpleWeb::StatusCode::success_ok);

      auto phase_4 = next_phase("clientpairingsecret", moonlight_data.client_pairing_secret);
      INFO(xml_to_str(phase_4.xml));
      REQUIRE(phase_4.status == SimpleWeb::StatusCode::success_ok);
      REQUIRE_THAT(app_state->config->paired_clients->load().get(), Catch::Matchers::SizeIs(1));
      REQUIRE(app_state->config->paired_clients->load().get()[0]->client_cert ==
              crypto::hex_to_str(moonlight_data.client_cert_base64, true));

      // Replaying the last phase must not pair again
      require_session_gone(next_phase("clientpairingsecret", moonlight_data.client_pairing_secret));
    }

    SECTION("a failed phase 4") {
      start_pairing();
      REQUIRE(next_phase("clientchallenge", moonlight_data.client_challenge).status ==
              SimpleWeb::StatusCode::success_ok);
      REQUIRE(next_phase("serverchallengeresp", moonlight_data.server_challenge_resp).status ==
              SimpleWeb::StatusCode::success_ok);
      // The server challenge is random here, so the client hash check fails
      REQUIRE(next_phase("clientpairingsecret", moonlight_data.client_pairing_secret).status ==
              SimpleWeb::StatusCode::client_error_bad_request);
      REQUIRE_THAT(app_state->config->paired_clients->load().get(), Catch::Matchers::IsEmpty());

      require_session_gone(next_phase("clientpairingsecret", moonlight_data.client_pairing_secret));
    }

    SECTION("a query matching no phase leaves the session alone") {
      start_pairing();
      REQUIRE_FALSE(endpoints::pair_next_phase(app_state, {{"foo", "bar"}}, "1234", "0.0.0.0").has_value());
      REQUIRE(app_state->pairing_cache->load().get().find(cache_key) != nullptr);
    }
  }

  SECTION("Testing null params") {
    auto phase_1 = endpoints::pair_phase1(app_state, "0.0.0.0", "1.1.1.1", {}, {}, cache_key);
    auto result = phase_1->get_future().get();
    REQUIRE(result.status == SimpleWeb::StatusCode::success_ok); // There's no reason to fail here

    auto client_cache = *(app_state->pairing_cache->load().get().find(cache_key));
    auto phase_2 = endpoints::pair_phase2(app_state, client_cache, {}, cache_key);
    REQUIRE(phase_2.status == SimpleWeb::StatusCode::success_ok); // Again, no reason to fail

    auto phase_3 = endpoints::pair_phase3(app_state, client_cache, {}, cache_key);
    REQUIRE(phase_3.status == SimpleWeb::StatusCode::success_ok); // Same, no reason to fail

    auto phase_4 = endpoints::pair_phase4(client_cache, {});
    REQUIRE(phase_4.status == SimpleWeb::StatusCode::client_error_bad_request); // certificate check

    client_cache.client_cert = crypto::hex_to_str(moonlight_data.client_cert_base64, true);
    client_cache.last_phase = state::PAIR_PHASE::SERVERCHALLENGERESP;
    phase_4 = endpoints::pair_phase4(client_cache, {});
    REQUIRE(phase_4.status == SimpleWeb::StatusCode::client_error_bad_request); // signature check
  }
}

TEST_CASE("applist", "[MoonlightProtocol]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto cfg = state::load_or_default("config.test.toml", event_bus, running_sessions);
  auto moonlight_profile = state::get_moonlight_profile(cfg);
  REQUIRE(moonlight_profile);
  immer::vector<immer::box<events::App>> apps = moonlight_profile.value()->apps->load();
  auto base_apps = apps | views::transform([](auto app) { return app->base; }) | to<immer::vector<moonlight::App>>();
  auto result = applist(base_apps);
  REQUIRE(xml_to_str(result) ==
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
          "<root status_code=\"200\">"
          "<App><IsHdrSupported>0</IsHdrSupported><AppTitle>Firefox</AppTitle><ID>304556286</ID></App>"
          "<App><IsHdrSupported>0</IsHdrSupported><AppTitle>Test ball</AppTitle><ID>378473508</ID></App>"
          "</root>");
}

TEST_CASE("launch", "[MoonlightProtocol]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto cfg = state::load_or_default("config.test.toml", event_bus, running_sessions);
  auto result = launch_success("192.168.1.1", "3021");
  REQUIRE(xml_to_str(result) == "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                "<root status_code=\"200\">"
                                "<sessionUrl0>rtsp://192.168.1.1:3021</sessionUrl0>"
                                "<gamesession>1</gamesession>"
                                "</root>");
}

TEST_CASE("Multiple users", "[HTTP]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto paired_clients = std::shared_ptr<immer::atom<state::PairedClientList>>();
  auto app_state = state::AppState{
      .config = state::Config{.paired_clients = paired_clients},
      .host = {},
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .event_bus = event_bus,
      .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>()};

  auto client1 = state::PairedClient{.app_state_folder = "test"};
  auto app1 = events::App{.base = moonlight::App{.title = "test_app"}};
  auto client1_ip = "0.0.0.0";
  auto client1_headers = SimpleWeb::CaseInsensitiveMultimap{{"rikey", "1234"}, {"rikeyid", "5678"}};
  auto session1 = endpoints::https::create_run_session(client1_headers, client1_ip, client1, app_state, app1);

  REQUIRE(session1->video_stream_port == 48100);
  REQUIRE(session1->audio_stream_port == 48200);

  app_state.running_sessions->update([session1](auto &sessions) { return sessions.push_back(*session1); });
  auto session2 = endpoints::https::create_run_session(client1_headers, client1_ip, client1, app_state, app1);

  REQUIRE(session2->video_stream_port == 48100);
  REQUIRE(session2->audio_stream_port == 48200);

  // Saving only the second session
  app_state.running_sessions->update(
      [session2](auto &sessions) { return immer::vector<events::StreamSession>{*session2}; });
  // We should now assign back the now available [48100, 48200] ports
  auto session3 = endpoints::https::create_run_session(client1_headers, client1_ip, client1, app_state, app1);

  REQUIRE(session3->video_stream_port == 48100);
  REQUIRE(session3->audio_stream_port == 48200);

  // Saving all 3 sessions (even if one is a duplicate)
  app_state.running_sessions->update([session1, session2, session3](auto &sessions) {
    return immer::vector<events::StreamSession>{*session1, *session2, *session3};
  });
  // We should now assign the 2nd port (even if we have 3 sessions) because of port clash
  auto session4 = endpoints::https::create_run_session(client1_headers, client1_ip, client1, app_state, app1);

  REQUIRE(session4->video_stream_port == 48100);
  REQUIRE(session4->audio_stream_port == 48200);
}

TEST_CASE("Launch rejects malformed client parameters", "[HTTP]") {
  auto app_state = state::AppState{
      .config = state::Config{.paired_clients = std::shared_ptr<immer::atom<state::PairedClientList>>()},
      .host = {},
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .event_bus = std::make_shared<events::EventBusType>(),
      .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>()};
  auto client = state::PairedClient{.app_state_folder = "test"};
  auto app = events::App{.base = moonlight::App{.title = "test_app"}};
  auto launch = [&](SimpleWeb::CaseInsensitiveMultimap headers) {
    headers.emplace("rikey", "1234");
    return endpoints::https::create_run_session(headers, "0.0.0.0", client, app_state, app);
  };

  // Moonlight sends rikeyid as a signed int, so negative values must keep working
  auto ok = launch({{"rikeyid", "-5678"}, {"mode", "1280x720x120"}, {"surroundAudioInfo", "4128774"}});
  REQUIRE(ok);
  REQUIRE(ok->display_mode.width == 1280);
  REQUIRE(ok->display_mode.height == 720);
  REQUIRE(ok->display_mode.refreshRate == 120);
  REQUIRE(ok->audio_channel_count == 6);

  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "1920"}}));  // used to index past the split
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "axbxc"}})); // used to throw std::invalid_argument
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "99999999999x1x1"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "1920x1080"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "1920x1080x"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "1920x1080x60x1"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "1920x1080x60fps"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "-1920x1080x60"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "0x0x0"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "0x1080x60"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "1920x0x60"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "1920x1080x0"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "16385x1080x60"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "1920x16385x60"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "1920x1080x1001"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"mode", "2147483647x2147483647x2147483647"}}));
  REQUIRE(launch({{"rikeyid", "1"}, {"mode", "16384x16384x1000"}})); // the bounds themselves are accepted
  REQUIRE(launch({{"rikeyid", "1"}, {"mode", "1x1x1"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"surroundAudioInfo", "abc"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "1"}, {"surroundAudioInfo", "196610junk"}}));
  REQUIRE_FALSE(launch({{"rikeyid", "abc"}}));
  REQUIRE_FALSE(launch({})); // missing rikeyid
}

TEST_CASE("HTTP Pairing over the /pair handler", "[MoonlightProtocol]") {
  PairingData moonlight_data;
  ServerData server_data;

  immer::box<state::AppState> app_state = {state::AppState{
      .config = state::Config{.config_source = "config.test.PAIRING.toml",
                              .paired_clients = std::make_shared<immer::atom<state::PairedClientList>>()},
      .host = {state::Host{.server_cert = server_data.server_cert,
                           .server_pkey = signature::create_key(server_data.server_pkey, true)}},
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = std::make_shared<events::EventBusType>()}};
  auto pair_handler = app_state->event_bus->register_handler<immer::box<events::PairSignal>>(
      [moonlight_data](const immer::box<events::PairSignal> &signal) {
        signal->user_pin->set_value(moonlight_data.pin);
      });

  SimpleWeb::Server<SimpleWeb::HTTP> server;
  server.config.address = "127.0.0.1";
  server.config.port = 0;
  server.resource["^/pair$"]["GET"] = [&app_state](auto resp, auto req) { endpoints::pair(resp, req, app_state); };
  std::promise<unsigned short> bound_port;
  std::thread server_thread([&] { server.start([&](unsigned short port) { bound_port.set_value(port); }); });
  // Stops the server even when a REQUIRE below throws
  auto stop_server = std::shared_ptr<void>(nullptr, [&](void *) {
    server.stop();
    server_thread.join();
  });
  SimpleWeb::Client<SimpleWeb::HTTP> client("127.0.0.1:" + std::to_string(bound_port.get_future().get()));
  client.config.timeout = 5; // a handler that never replies fails the test instead of hanging it
  auto get = [&client](const std::string &query) {
    try {
      auto response = client.request("GET", "/pair?" + query);
      return std::make_pair(response->status_code, response->content.string());
    } catch (const std::exception &e) {
      return std::make_pair(std::string("no reply"), std::string(e.what()));
    }
  };

  // Phase 1: the handler answers once the user has entered the PIN
  auto [status_1, body_1] = get("uniqueid=1234&devicename=roth&updateState=1&phrase=getservercert&salt=" +
                                moonlight_data.salt + "&clientcert=" + moonlight_data.client_cert_base64);
  REQUIRE_THAT(status_1, Equals("200 OK"));
  REQUIRE_THAT(body_1, Catch::Matchers::ContainsSubstring("<paired>1</paired>"));

  // Phase 2 goes through pair_next_phase: the handler must send its reply
  auto [status_2, body_2] =
      get("uniqueid=1234&devicename=roth&updateState=1&clientchallenge=" + moonlight_data.client_challenge);
  REQUIRE_THAT(status_2, Equals("200 OK"));
  REQUIRE_THAT(body_2, Catch::Matchers::ContainsSubstring("<challengeresponse>"));

  // Requests that match no phase are answered too
  auto [status_3, body_3] = get("uniqueid=1234&foo=bar");
  REQUIRE_THAT(status_3, Equals("400 Bad Request"));
  REQUIRE_THAT(body_3, Catch::Matchers::ContainsSubstring("<paired>0</paired>"));
  auto [status_4, body_4] = get("salt=" + moonlight_data.salt);
  REQUIRE_THAT(status_4, Equals("400 Bad Request"));
}

TEST_CASE("HTTPS /resume carries the stream's devices over to the new instance", "[MoonlightProtocol]") {
  // Certificates for the HTTPS server and the (already paired) client
  char dir_template[] = "/tmp/wolf-resume-XXXXXX";
  REQUIRE(mkdtemp(dir_template));
  std::string dir = dir_template;
  auto server_key = x509::generate_key();
  REQUIRE(x509::write_to_disk(server_key, dir + "/server.key", x509::generate_x509(server_key), dir + "/server.crt"));
  auto client_key = x509::generate_key();
  auto client_cert = x509::generate_x509(client_key);
  REQUIRE(x509::write_to_disk(client_key, dir + "/client.key", client_cert, dir + "/client.crt"));

  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = state::Config{.paired_clients = std::make_shared<immer::atom<state::PairedClientList>>()},
      .host = {},
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .event_bus = std::make_shared<events::EventBusType>(),
      .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>()});
  auto client = state::PairedClient{.client_cert = x509::get_cert_pem(client_cert), .app_state_folder = "test"};
  auto app = events::App{.base = moonlight::App{.title = "test_app"}};

  // The running stream, with the devices and producers the resumed one must keep
  auto old_session =
      endpoints::https::create_run_session({{"rikey", "1234"}, {"rikeyid", "1"}}, "127.0.0.1", client, app_state, app);
  REQUIRE(old_session);
  old_session->producer_name = "the_old_producers";
  auto kept = *old_session; // its shared state, which the resume moves out of the old session
  app_state->running_sessions->store(immer::vector{*old_session});

  HttpsServer server(dir + "/server.crt", dir + "/server.key");
  server.config.address = "127.0.0.1";
  server.config.port = 0;
  server.resource["^/resume$"]["GET"] = [&](auto resp, auto req) {
    endpoints::https::resume(resp, req, client, app_state);
  };
  std::promise<unsigned short> bound_port;
  std::thread server_thread([&] { server.start([&](unsigned short port) { bound_port.set_value(port); }); });
  auto stop_server = std::shared_ptr<void>(nullptr, [&](void *) {
    server.stop();
    server_thread.join();
    std::filesystem::remove_all(dir);
  });
  SimpleWeb::Client<SimpleWeb::HTTPS> https_client("127.0.0.1:" + std::to_string(bound_port.get_future().get()),
                                                   false,
                                                   dir + "/client.crt",
                                                   dir + "/client.key");
  https_client.config.timeout = 5;
  auto response = https_client.request("GET", "/resume?rikey=5678&rikeyid=2&surroundAudioInfo=196610");
  REQUIRE_THAT(response->status_code, Equals("200 OK"));

  auto sessions = app_state->running_sessions->load();
  REQUIRE(sessions->size() == 1);
  auto resumed = sessions->front();
  REQUIRE(resumed.aes_key != kept.aes_key); // a new stream instance...
  REQUIRE(resumed.session_id == kept.session_id);
  // ...that keeps the old one's desktop, producers, audio sink and devices
  REQUIRE(resumed.wayland_display == kept.wayland_display);
  REQUIRE(resumed.producer_name == "the_old_producers");
  REQUIRE(resumed.audio_sink == kept.audio_sink);
  REQUIRE(resumed.mouse == kept.mouse);
  REQUIRE(resumed.keyboard == kept.keyboard);
  REQUIRE(resumed.joypads == kept.joypads);
  REQUIRE(resumed.control_feedback == kept.control_feedback); // the joypads' feedback reaches the new peer
  REQUIRE(resumed.pen_tablet == kept.pen_tablet);
  REQUIRE(resumed.touch_screen == kept.touch_screen);
  // Not carried over: the new instance negotiates its own control stream
  REQUIRE(resumed.control_stream != kept.control_stream);
}
