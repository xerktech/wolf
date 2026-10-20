#include <api/api.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <core/docker.hpp>
#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <poll.h>
#include <tuple>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <helpers/tsqueue.hpp>
#include <helpers/utils.hpp>
#include <rfl/toml.hpp>
#include <sessions/handlers.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>
#include <sys/stat.h>
#include <unistd.h>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Equals;

using namespace wolf::api;
using curl_ptr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

/**
 * The API tests' runtime dir, one per wolftests run.
 * Not XDG_RUNTIME_DIR: a long one overflows sun_path (~107 chars) and aborts the whole binary, and a shared one
 * (e.g. /tmp) lets concurrent runs on one host reach each other's wolf.sock.
 */
std::string api_runtime_dir() {
  static const std::string runtime_dir = []() {
    char runtime_dir_template[] = "/tmp/wolf-api-XXXXXX";
    if (!mkdtemp(runtime_dir_template)) {
      throw std::runtime_error("Unable to create the API tests' runtime dir");
    }
    return std::string(runtime_dir_template);
  }();
  return runtime_dir;
}

std::string api_socket_path() {
  return (std::filesystem::path(api_runtime_dir()) / "wolf.sock").string();
}

/**
 * Start the API server and wait until it accepts connections.
 * Servers started by earlier tests keep running: their socket file is removed first, so that we can't reach one of
 * them instead of the new server.
 */
void start_api_server(const immer::box<state::AppState> &app_state) {
  auto socket_path = api_socket_path();
  std::filesystem::remove(socket_path);
  std::thread([app_state, runtime_dir = api_runtime_dir()]() {
    wolf::api::start_server(runtime_dir, app_state);
  }).detach();
  for (int attempt = 0; attempt < 500; attempt++) {
    boost::asio::io_context ctx;
    boost::asio::local::stream_protocol::socket probe(ctx);
    boost::system::error_code ec;
    probe.connect(boost::asio::local::stream_protocol::endpoint(socket_path), ec);
    if (!ec) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  FAIL("The API server didn't start");
}

/**
 * Perform a HTTP request using curl
 */
std::optional<std::pair<long /* response_code */, std::string /* raw message */>>
req(CURL *handle,
    HTTPMethod method,
    std::string_view target,
    std::string_view post_body = {},
    const std::vector<std::string> &header_params = {}) {
  logs::log(logs::trace, "[CURL] Sending [{}] -> {}", (int)method, target);
  curl_easy_setopt(handle, CURLOPT_URL, target.data());

  /* Set method */
  switch (method) {
  case HTTPMethod::GET:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "GET");
    break;
  case HTTPMethod::POST:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "POST");
    break;
  case HTTPMethod::PUT:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PUT");
    break;
  case HTTPMethod::DELETE:
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
    break;
  }
  curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

  struct curl_slist *headers = nullptr;
  for (const auto &header : header_params) {
    headers = curl_slist_append(headers, header.c_str());
  }

  /* Pass POST params (if present) */
  if (method == HTTPMethod::POST && !post_body.empty()) {
    logs::log(logs::trace, "[CURL] POST: {}", post_body.substr(0, 1024));

    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    headers = curl_slist_append(headers, "Content-type: application/json");
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, post_body.data());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, post_body.size());
  } else {
    curl_easy_setopt(handle, CURLOPT_POST, 0L);
  }
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

  /* Set custom writer (in order to receive back the response) */
  curl_easy_setopt(
      handle,
      CURLOPT_WRITEFUNCTION,
      static_cast<size_t (*)(char *, size_t, size_t, void *)>([](char *ptr, size_t size, size_t nmemb, void *read_buf) {
        *(static_cast<std::string *>(read_buf)) += std::string{ptr, size * nmemb};
        return size * nmemb;
      }));
  std::string read_buf;
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &read_buf);

  /* Run! */
  auto res = curl_easy_perform(handle);
  curl_slist_free_all(headers);
  if (res != CURLE_OK) {
    logs::log(logs::warning, "[CURL] Request failed with error: {}", curl_easy_strerror(res));
    return {};
  } else {
    long response_code;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
    // Avoid printing binary output if the target is "/api/v1/utils/get-icon"
    if (target.find("/api/v1/utils/get-icon") == std::string::npos) {
      logs::log(logs::trace, "[CURL] Received {} - {}", response_code, std::string_view(read_buf).substr(0, 1024));
    }
    return {{response_code, read_buf}};
  }
}

TEST_CASE("Pair APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = state::load_or_default("config.test.toml", event_bus, running_sessions);
  { // Avoid overriding the test config file (shared across multiple tests)
    config.config_source = "config.test.EDITED.toml";
    auto tml = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>("config.test.toml").value();
    rfl::toml::save(config.config_source, tml);
  }
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = immer::box<state::Config>(config),
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  start_api_server(app_state);

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/pair/pending");
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true,\"requests\":[]}"));

  // Checkout the list of paired clients (there will be one in the test config file)
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/clients");
  REQUIRE(response);
  REQUIRE_THAT(response->second,
               Equals("{\"success\":true,\"clients\":["
                      "{\"client_id\":\"10594003729173467913\","
                      "\"app_state_folder\":\"some/folder\","
                      "\"settings\":{"
                      "\"run_uid\":1234,"
                      "\"run_gid\":5678,"
                      "\"controllers_override\":[\"PS\"],"
                      "\"mouse_acceleration\":2.5,"
                      "\"v_scroll_acceleration\":1.5,"
                      "\"h_scroll_acceleration\":10.199999809265137,"
                      "\"motion_controller_override\":\"AUTO\"}}]}"));

  auto pair_promise = std::make_shared<boost::promise<std::string>>();

  // Simulate a Moonlight pairing request
  app_state->pairing_atom->update([pair_promise](auto pairing_map) {
    return pairing_map.set("secret",
                           immer::box<events::PairSignal>{
                               events::PairSignal{.client_ip = "1234", .host_ip = "5678", .user_pin = pair_promise}});
  });

  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/pair/pending");
  REQUIRE(response);
  REQUIRE_THAT(response->second,
               Equals("{\"success\":true,\"requests\":[{\"pair_secret\":\"secret\",\"client_ip\":\"1234\"}]}"));

  // Let's complete the pairing process
  response = req(curl.get(),
                 HTTPMethod::POST,
                 "http://localhost/api/v1/pair/client",
                 "{\"pair_secret\":\"secret\",\"pin\":\"1234\"}");
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
  REQUIRE(pair_promise->get_future().get() == "1234");
  REQUIRE(app_state->config.get().paired_clients->load().get().size() == 1);

  { // Test out if pairing with the same client_id overrides the previous client
    // https://github.com/games-on-whales/wolf/issues/211
    auto cfg = app_state->config.get();
    immer::box<state::PairedClient> previous_client = cfg.paired_clients->load().get()[0];
    state::pair(
        cfg,
        state::PairedClient{.client_cert = previous_client->client_cert, .app_state_folder = "ASDF", .settings = {}});
    REQUIRE(cfg.paired_clients->load().get().size() == 1);
    REQUIRE(cfg.paired_clients->load().get()[0]->app_state_folder == "ASDF");
  }

  REQUIRE(app_state->pairing_atom->load().get().size() == 0);

  { // Test out changing client settings
    response = req(curl.get(),
                   HTTPMethod::POST,
                   "http://localhost/api/v1/clients/settings",
                   "{\"client_id\":\"10594003729173467913\",\"app_state_folder\":\"OVERRIDDEN\", \"settings\":{}}");
    REQUIRE(response);
    REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
    REQUIRE_THAT(app_state->config.get().paired_clients->load().get()[0]->app_state_folder, Equals("OVERRIDDEN"));

    // Check back that we've correctly updated the config file
    auto tml = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>(config.config_source).value();
    REQUIRE(tml.paired_clients[0].app_state_folder == "OVERRIDDEN");
  }

  { // Test out unpairing
    response = req(curl.get(),
                   HTTPMethod::POST,
                   "http://localhost/api/v1/unpair/client",
                   "{\"client_id\":\"10594003729173467913\"}");
    REQUIRE(response);
    REQUIRE_THAT(response->second, Equals("{\"success\":true}"));

    // Check back that we've correctly updated the config file
    auto tml = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>(config.config_source).value();
    REQUIRE(tml.paired_clients.empty());
  }
}

TEST_CASE("APPs APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = state::load_or_default("config.test.toml", event_bus, running_sessions);
  // Avoid overriding the test config file (shared across multiple tests)
  config.config_source = "config.test.EDITED.toml";
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = {config},
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  start_api_server(app_state);

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  // Test that the initial list of apps matches what's in the test config file
  auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/apps");
  REQUIRE(response);
  auto apps = rfl::json::read<AppListResponse>(response->second).value();
  REQUIRE(apps.success);
  REQUIRE(apps.apps.size() == 2);
  REQUIRE(apps.apps[0].title == "Firefox");
  REQUIRE(apps.apps[1].title == "Test ball");
  REQUIRE(apps.apps[0].video_producer_buffer_caps.has_value());
  REQUIRE_FALSE(apps.apps[0].video_producer_buffer_caps->empty());

  // Test that we can add an app
  auto app = rfl::Reflector<wolf::core::events::App>::ReflType{
      .title = "Test app",
      .id = "test",
      .support_hdr = false,
      .video_producer_buffer_caps = "video/x-raw(memory:DMABuf)",
      .h264_gst_pipeline = "h264",
      .hevc_gst_pipeline = "hevc",
      .av1_gst_pipeline = "av1",
      .render_node = "render",
      .opus_gst_pipeline = "opus",
      .start_virtual_compositor = false,
      .runner = wolf::config::AppDocker{.name = "test",
                                        .image = "test",
                                        .mounts = {"/tmp:/tmp"},
                                        .env = {"LOG_LEVEL=1234"},
                                        .devices = {"/dev/input:/dev/input"},
                                        .ports = {"8080:8080"}}};
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/apps/add", rfl::json::write(app));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
  // Adding it again (e.g. a client retrying) doesn't create a second app with the same id
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/apps/add", rfl::json::write(app));
  REQUIRE(response);
  REQUIRE(response->first == 409);

  // Test that the new app is in the list
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/apps");
  REQUIRE(response);
  auto apps2 = rfl::json::read<AppListResponse>(response->second).value();
  REQUIRE(apps2.success);
  REQUIRE(apps2.apps.size() == 3);
  REQUIRE(apps2.apps[2].title == "Test app");
  REQUIRE(apps2.apps[2].video_producer_buffer_caps == "video/x-raw(memory:DMABuf)");
  auto moonlight_profile = state::get_moonlight_profile(config);
  REQUIRE(moonlight_profile);
  immer::vector<immer::box<events::App>> wolf_apps = moonlight_profile.value()->apps->load();
  REQUIRE(wolf_apps.at(2)->base.title == "Test app");
  REQUIRE(wolf_apps.at(2)->video_producer_buffer_caps == "video/x-raw(memory:DMABuf)");

  // Test that apps survive a restart: reloading the saved config gives back the same apps
  {
    auto reloaded = state::load_or_default(config.config_source, event_bus, running_sessions);
    immer::vector<immer::box<events::App>> reloaded_apps = state::get_moonlight_profile(reloaded).value()->apps->load();
    REQUIRE(reloaded_apps.size() == 3);
    for (auto i = 0; i < 3; i++) {
      REQUIRE(reloaded_apps.at(i)->base.title == wolf_apps.at(i)->base.title);
      REQUIRE(reloaded_apps.at(i)->base.id == wolf_apps.at(i)->base.id);
      REQUIRE(reloaded_apps.at(i)->video_producer_buffer_caps == wolf_apps.at(i)->video_producer_buffer_caps);
      REQUIRE(reloaded_apps.at(i)->h264_gst_pipeline == wolf_apps.at(i)->h264_gst_pipeline);
      REQUIRE(reloaded_apps.at(i)->opus_gst_pipeline == wolf_apps.at(i)->opus_gst_pipeline);
      REQUIRE(reloaded_apps.at(i)->render_node == wolf_apps.at(i)->render_node);
      REQUIRE(reloaded_apps.at(i)->start_virtual_compositor == wolf_apps.at(i)->start_virtual_compositor);
    }
    REQUIRE(reloaded_apps.at(2)->base.id == "test");
    REQUIRE(reloaded_apps.at(2)->video_producer_buffer_caps == "video/x-raw(memory:DMABuf)");
    REQUIRE(reloaded_apps.at(2)->h264_gst_pipeline == "h264");
    REQUIRE(reloaded_apps.at(2)->opus_gst_pipeline == "opus");
    if (!reloaded_apps.at(0)->hevc_gst_pipeline.empty()) { // HEVC encoder available
      REQUIRE(reloaded_apps.at(2)->hevc_gst_pipeline == "hevc");
    }
    if (!reloaded_apps.at(0)->av1_gst_pipeline.empty()) { // AV1 encoder available
      REQUIRE(reloaded_apps.at(2)->av1_gst_pipeline == "av1");
    }

    // Apps from the config file keep their entry as written, and defaults aren't frozen into the file
    auto tml = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>(config.config_source).value();
    auto tml_profile = std::find_if(tml.profiles.begin(), tml.profiles.end(), [](const auto &p) {
      return p.id == events::MOONLIGHT_PROFILE_ID;
    });
    REQUIRE(tml_profile != tml.profiles.end());
    REQUIRE_FALSE(tml_profile->apps.at(0).video.has_value());
    REQUIRE_FALSE(tml_profile->apps.at(0).id.has_value());
    auto pristine = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>("config.test.toml").value();
    auto pristine_profile = std::find_if(pristine.profiles.begin(), pristine.profiles.end(), [](const auto &p) {
      return p.id == events::MOONLIGHT_PROFILE_ID;
    });
    REQUIRE(rfl::json::write(tml_profile->apps.at(0)) == rfl::json::write(pristine_profile->apps.at(0)));
    REQUIRE(rfl::json::write(tml_profile->apps.at(1)) == rfl::json::write(pristine_profile->apps.at(1)));
    REQUIRE(tml_profile->apps.at(1).video->source == "override DEFAULT SOURCE");
    REQUIRE_FALSE(tml_profile->apps.at(1).video->h264_pipeline.has_value());
    REQUIRE(tml_profile->apps.at(2).id == "test");
    REQUIRE_FALSE(tml_profile->apps.at(2).video->source.has_value());
  }

  // An app added with this host's default pipelines doesn't get them frozen into the config file
  {
    auto firefox = rfl::Reflector<events::App>::from(wolf_apps.at(0));
    auto defaults_app = rfl::Reflector<events::App>::ReflType{
        .title = "Defaults app",
        .id = "defaults",
        .support_hdr = false,
        .video_producer_buffer_caps = firefox.video_producer_buffer_caps,
        .h264_gst_pipeline = firefox.h264_gst_pipeline,
        .hevc_gst_pipeline = firefox.hevc_gst_pipeline,
        .av1_gst_pipeline = firefox.av1_gst_pipeline,
        .render_node = firefox.render_node,
        .opus_gst_pipeline = firefox.opus_gst_pipeline,
        .start_virtual_compositor = true,
        .start_audio_server = true,
        .runner = firefox.runner};
    response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/apps/add", rfl::json::write(defaults_app));
    REQUIRE(response);
    REQUIRE_THAT(response->second, Equals("{\"success\":true}"));

    auto tml = rfl::toml::load<wolf::config::WolfConfig, rfl::DefaultIfMissing>(config.config_source).value();
    auto tml_profile = std::find_if(tml.profiles.begin(), tml.profiles.end(), [](const auto &p) {
      return p.id == events::MOONLIGHT_PROFILE_ID;
    });
    REQUIRE(tml_profile->apps.at(3).title == "Defaults app");
    REQUIRE(tml_profile->apps.at(3).id == "defaults");
    REQUIRE_FALSE(tml_profile->apps.at(3).video.has_value());
    REQUIRE_FALSE(tml_profile->apps.at(3).audio.has_value());
    REQUIRE_FALSE(tml_profile->apps.at(3).render_node.has_value());

    response = req(curl.get(),
                   HTTPMethod::POST,
                   "http://localhost/api/v1/apps/delete",
                   rfl::json::write(AppDeleteRequest{.id = "defaults"}));
    REQUIRE(response);
  }

  // Test that we can remove an app
  auto app_delete = AppDeleteRequest{.id = "test"};
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/apps/delete", rfl::json::write(app_delete));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));

  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/apps");
  REQUIRE(response);
  auto apps3 = rfl::json::read<AppListResponse>(response->second).value();
  REQUIRE(apps3.success);
  REQUIRE(apps3.apps.size() == 2);

  // Apps added without (or with blank) producer caps fall back to video/x-raw
  for (const auto &caps : {std::optional<std::string>{},
                           std::optional<std::string>{""},
                           std::optional<std::string>{" "},
                           std::optional<std::string>{"\t\n"},
                           std::optional<std::string>{"\v\f"}}) {
    auto default_app = app;
    default_app.video_producer_buffer_caps = caps;
    response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/apps/add", rfl::json::write(default_app));
    REQUIRE(response);
    REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
    wolf_apps = moonlight_profile.value()->apps->load();
    REQUIRE(wolf_apps.at(2)->video_producer_buffer_caps == "video/x-raw");
    response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/apps/delete", rfl::json::write(app_delete));
    REQUIRE(response);
  }
  // Replies bigger than a single socket write must arrive intact
  auto big_app = app;
  big_app.h264_gst_pipeline = std::string(4 * 1024 * 1024, 'x');
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/apps/add", rfl::json::write(big_app));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
#ifdef __GLIBC__
  mallopt(M_PERTURB, 165); // scrub freed memory, so a reply sent from a freed buffer is reliably corrupted
#endif
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/apps");
#ifdef __GLIBC__
  mallopt(M_PERTURB, 0);
#endif
  REQUIRE(response);
  auto apps4 = rfl::json::read<AppListResponse>(response->second);
  REQUIRE(apps4);
  REQUIRE(apps4.value().apps.size() == 3);
  REQUIRE(apps4.value().apps[2].h264_gst_pipeline == big_app.h264_gst_pipeline);
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/apps/delete", rfl::json::write(app_delete));
  REQUIRE(response);
}

TEST_CASE("Profile APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = state::load_or_default("config.test.toml", event_bus, running_sessions);
  // Avoid overriding the test config file (shared across multiple tests)
  config.config_source = "config.test.EDITED.toml";
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = {config},
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  start_api_server(app_state);

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  // Test the initial profile list only has the default user
  {
    auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/profiles");
    REQUIRE(response);
    REQUIRE_THAT(response->second,
                 Catch::Matchers::Equals(
                     R"({"success":true,"profiles":[{"name":"User","id":"user","icon_png_path":"","apps":[]}]})"));
  }

  // Test adding a profile
  {
    auto runner = std::make_shared<process::RunProcess>(event_bus, "destroy-all-humans.exe");
    auto apps = immer::vector<immer::box<events::App>>{
        events::App{.base = {.title = "Destroy All Humans", .id = "2", .support_hdr = false, .icon_png_path = ""},
                    .h264_gst_pipeline = "h264",
                    .hevc_gst_pipeline = "hevc",
                    .av1_gst_pipeline = "av1",
                    .render_node = "render_node",
                    .opus_gst_pipeline = "opus",
                    .start_virtual_compositor = true,
                    .start_audio_server = false,
                    .runner = runner}};
    auto profile = events::Profile{
        .id = "test",
        .name = "Test",
        .icon_png_path = "",
        .apps = std::make_shared<immer::atom<immer::vector<immer::box<events::App>>>>(apps),
    };
    auto response =
        req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/profiles/add", rfl::json::write(profile));
    REQUIRE(response);
    REQUIRE_THAT(response->second, Catch::Matchers::Equals(R"({"success":true})"));

    // Test the profile list now has the new profile
    auto response2 = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/profiles");
    REQUIRE(response2);
    auto profiles = rfl::json::read<ProfileListResponse>(response2->second);
    REQUIRE(profiles);
    REQUIRE(profiles.value().profiles.size() == 2);
    REQUIRE_THAT(profiles.value().profiles[1].name, Equals(profile.name));
    REQUIRE_THAT(profiles.value().profiles[1].id, Equals(profile.id));
    REQUIRE_THAT(profiles.value().profiles[1].icon_png_path, Equals(profile.icon_png_path));
    REQUIRE(profiles.value().profiles[1].apps.size() == 1);
  }

  // Test removing a profile
  {
    auto response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/profiles/remove", R"({"id":"test"})");
    REQUIRE(response);
    REQUIRE_THAT(response->second, Catch::Matchers::Equals(R"({"success":true})"));
    // Test the profile list now has the new profile
    auto response2 = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/profiles");
    REQUIRE(response2);
    auto profiles = rfl::json::read<ProfileListResponse>(response2->second);
    REQUIRE(profiles);
    REQUIRE(profiles.value().profiles.size() == 1);
    REQUIRE_THAT(profiles.value().profiles[0].name, Equals("User"));
  }
}

TEST_CASE("Sessions APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  start_api_server(app_state);

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  // Test that the initial list of sessions is empty
  auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  REQUIRE(response);
  auto sessions = rfl::json::read<StreamSessionListResponse>(response->second).value();
  REQUIRE(sessions.success);
  REQUIRE(sessions.sessions.size() == 0);

  // Test that we can add a session
  auto session = rfl::Reflector<wolf::core::events::StreamSession>::ReflType{
      .client_ip = "127.0.0.1",
      .aes_key = "00112233445566778899aabbccddeeff",
      .aes_iv = "1",
      .video_width = 1920,
      .video_height = 1080,
      .video_refresh_rate = 60,
      .app_id = "304556286",               // test cfg file
      .client_id = "10594003729173467913", // test cfg file
  };

  // A zero or huge display mode is refused, as on the Moonlight /launch path, and nothing is stored
  for (auto [width, height, fps] : {std::tuple{0, 1080, 60},
                                    std::tuple{1920, 0, 60},
                                    std::tuple{1920, 1080, 0},
                                    std::tuple{-1920, 1080, 60},
                                    std::tuple{16385, 1080, 60},
                                    std::tuple{1920, 16385, 60},
                                    std::tuple{1920, 1080, 1001},
                                    std::tuple{2147483647, 2147483647, 2147483647}}) {
    auto bad_session = session;
    bad_session.video_width = width;
    bad_session.video_height = height;
    bad_session.video_refresh_rate = fps;
    response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(bad_session));
    REQUIRE(response);
    REQUIRE(response->first == 400);
    REQUIRE_THAT(response->second, ContainsSubstring("\"success\":false"));
  }
  REQUIRE(running_sessions->load()->size() == 0);
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Catch::Matchers::ContainsSubstring("{\"success\":true,\"session_id\":"));

  // Test that the new session is in the list
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  REQUIRE(response);
  auto sessions2 = rfl::json::read<StreamSessionListResponse>(response->second).value();
  REQUIRE(sessions2.success);
  REQUIRE(sessions2.sessions.size() == 1);
  // The session fields stay at the top level, next to its state
  REQUIRE(sessions2.sessions[0].session.get().client_ip == "127.0.0.1");
  REQUIRE_THAT(response->second, ContainsSubstring("\"client_ip\":\"127.0.0.1\""));
  REQUIRE(sessions2.sessions[0].state.name() == "setup");

  // The listed state follows the stream's: streaming once both pipelines run, paused after a pause
  auto stream_state = running_sessions->load()->front().stream_state;
  auto listed_state = [&]() {
    auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
    REQUIRE(response);
    return rfl::json::read<StreamSessionListResponse>(response->second).value().sessions[0].state.name();
  };
  stream_state->update([](auto state) {
    state.video_pipelines++;
    return state;
  });
  REQUIRE(listed_state() == "setup");
  stream_state->update([](auto state) {
    state.audio_pipelines++;
    return state;
  });
  REQUIRE(listed_state() == "streaming");
  stream_state->update([](auto state) {
    state.paused = true;
    return state;
  });
  REQUIRE(listed_state() == "paused");

  // TODO: breaks Github CI
  // // Test that we can send input to a session
  // auto input_request = StreamSessionHandleInputRequest{
  //     .session_id = "10594003729173467913",
  //     // example CONTROLLER_MULTI packet
  //     .input_packet_hex = "060222000000001E0C0000001A000000010014000010000000000000000000009C0000005500"};
  // response =
  //     req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/input", rfl::json::write(input_request));
  // REQUIRE(response);
  // REQUIRE_THAT(response->second, Equals("{\"success\":true}"));

  // Test that we can pause a session
  auto pause_request = StreamSessionPauseRequest{.session_id = "10594003729173467913"};
  response =
      req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/pause", rfl::json::write(pause_request));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));

  // Test that we can stop a session
  auto stop_request = StreamSessionStopRequest{.session_id = "10594003729173467913"};
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/stop", rfl::json::write(stop_request));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
}

/**
 * A stand-in for a stream's GStreamer pipeline: it runs until its session is paused or stopped, or until its stream
 * instance is superseded, then takes `teardown` to finish, as a real pipeline does while it drains to EOS.
 */
struct FakePipelines {
  std::shared_ptr<events::EventBusType> ev_bus;
  std::chrono::milliseconds teardown;
  std::shared_ptr<std::atomic_int> started = std::make_shared<std::atomic_int>(0);
  std::shared_ptr<std::atomic_int> ended = std::make_shared<std::atomic_int>(0);

  void run(std::size_t session_id, std::array<char, 16> rtp_secret) const {
    auto end_signal = std::make_shared<std::promise<void>>();
    auto end_future = end_signal->get_future();
    auto signalled = std::make_shared<std::atomic_bool>(false);
    auto end = [end_signal, signalled]() {
      if (!signalled->exchange(true)) {
        end_signal->set_value();
      }
    };
    auto pause = ev_bus->register_handler<immer::box<events::PauseStreamEvent>>(
        [=](const immer::box<events::PauseStreamEvent> &ev) {
          if (ev->session_id == session_id)
            end();
        });
    auto stop = ev_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [=](const immer::box<events::StopStreamEvent> &ev) {
          if (ev->session_id == session_id)
            end();
        });
    auto superseded = ev_bus->register_handler<immer::box<events::EndSupersededStreamEvent>>(
        [=](const immer::box<events::EndSupersededStreamEvent> &ev) {
          if (ev->session_id == session_id && ev->rtp_secret_payload == rtp_secret)
            end();
        });
    (*started)++;
    end_future.wait();
    pause.unregister();
    stop.unregister();
    superseded.unregister();
    std::this_thread::sleep_for(teardown);
    (*ended)++;
  }

  sessions::StreamPipelines pipelines() const {
    return {.video = [*this](const immer::box<events::VideoSession> &sess,
                             const auto &) { run(sess->session_id, sess->rtp_secret_payload); },
            .audio = [*this](const immer::box<events::AudioSession> &sess,
                             const auto &) { run(sess->session_id, sess->rtp_secret_payload); }};
  }
};

TEST_CASE("Sessions API: the listed state follows the stream's events", "[API]") {
  using namespace std::chrono_literals;
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto teardown = 500ms;
  auto fake = FakePipelines{.ev_bus = event_bus, .teardown = teardown};
  auto pipeline_handlers = sessions::setup_stream_pipeline_handlers(event_bus, running_sessions, fake.pipelines());

  // Each run (one per SECTION) gets its own socket: the previous run's server is still listening on its own.
  // Kept short under /tmp, whatever XDG_RUNTIME_DIR is, so the socket path fits in sockaddr_un.
  char runtime_dir_template[] = "/tmp/wolf-test-XXXXXX";
  REQUIRE(mkdtemp(runtime_dir_template));
  auto runtime_dir = std::string(runtime_dir_template);
  auto remove_runtime_dir = std::shared_ptr<void>(nullptr, [runtime_dir](void *) {
    std::error_code ec;
    std::filesystem::remove_all(runtime_dir, ec);
  });
  auto socket_path = (std::filesystem::path(runtime_dir) / "wolf.sock").string();
  std::thread server_thread([app_state, runtime_dir]() { wolf::api::start_server(runtime_dir, app_state); });
  server_thread.detach();

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);
  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  // Wait for the server to start
  auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  for (int i = 0; i < 250 && !response; i++) {
    std::this_thread::sleep_for(20ms);
    response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  }
  REQUIRE(response);

  auto session = rfl::Reflector<wolf::core::events::StreamSession>::ReflType{
      .client_ip = "127.0.0.1",
      .aes_key = "00112233445566778899aabbccddeeff",
      .aes_iv = "1",
      .video_width = 1920,
      .video_height = 1080,
      .video_refresh_rate = 60,
      .app_id = "304556286",               // test cfg file
      .client_id = "10594003729173467913", // test cfg file
  };
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
  REQUIRE(response);
  REQUIRE_THAT(response->second, ContainsSubstring("{\"success\":true,\"session_id\":"));
  REQUIRE(running_sessions->load()->size() == 1);
  auto session_id = running_sessions->load()->front().session_id;

  auto listed_state = [&]() {
    auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
    REQUIRE(response);
    auto sessions = rfl::json::read<StreamSessionListResponse>(response->second).value();
    REQUIRE(sessions.sessions.size() == 1);
    return std::string(sessions.sessions[0].state.name());
  };
  auto rtp_secret = [](char id) { return std::array<char, 16>{id}; };
  auto video_session = [&](char secret) {
    events::VideoSession video{.session_id = session_id};
    video.display_mode = {.width = 1920, .height = 1080, .refreshRate = 60};
    video.rtp_secret_payload = rtp_secret(secret);
    return video;
  };
  auto audio_session = [&](char secret) {
    events::AudioSession audio{.session_id = session_id};
    audio.rtp_secret_payload = rtp_secret(secret);
    return audio;
  };
  // A real client keeps pinging until its stream starts: the handlers start listening for pings on another thread
  auto ping_until_started = [&](char secret, int started) {
    for (int i = 0; i < 250 && *fake.started < started; i++) {
      event_bus->fire_event(immer::box<events::RTPVideoPingEvent>(
          events::RTPVideoPingEvent{.client_ip = "127.0.0.1", .client_port = 9, .payload = rtp_secret(secret)}));
      event_bus->fire_event(immer::box<events::RTPAudioPingEvent>(
          events::RTPAudioPingEvent{.client_ip = "127.0.0.1", .client_port = 9, .payload = rtp_secret(secret)}));
      std::this_thread::sleep_for(20ms);
    }
    REQUIRE(*fake.started == started);
  };
  // RTSP setup of a stream instance
  auto setup = [&](char secret) {
    event_bus->fire_event(immer::box<events::VideoSession>(video_session(secret)));
    event_bus->fire_event(immer::box<events::AudioSession>(audio_session(secret)));
  };

  REQUIRE(listed_state() == "setup");
  setup(1);
  REQUIRE(listed_state() == "setup"); // until the client's first ping
  // A pause before the first ping, with no pipeline to end yet, doesn't hold the stream paused once it starts
  event_bus->fire_event(immer::box<events::PauseStreamEvent>(events::PauseStreamEvent{.session_id = session_id}));
  REQUIRE(listed_state() == "paused");
  ping_until_started(1, 2);
  REQUIRE(listed_state() == "streaming");

  SECTION("an API start during a slow teardown after a pause stays streaming") {
    event_bus->fire_event(immer::box<events::PauseStreamEvent>(events::PauseStreamEvent{.session_id = session_id}));
    // The old pipelines are still tearing down, but the stream is paused
    REQUIRE(*fake.ended == 0);
    REQUIRE(listed_state() == "paused");

    auto start_request = StreamSessionStartRequest{.session_id = std::to_string(session_id),
                                                   .video_session = video_session(2),
                                                   .audio_session = audio_session(2)};
    response =
        req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/start", rfl::json::write(start_request));
    REQUIRE(response);
    REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
    REQUIRE(listed_state() != "paused"); // set up again, before its first ping
    ping_until_started(2, 4);
    REQUIRE(*fake.ended == 0);
    REQUIRE(listed_state() == "streaming");

    // The paused pipelines finishing their teardown doesn't end the restarted stream
    std::this_thread::sleep_for(teardown + 300ms);
    REQUIRE(*fake.ended == 2);
    REQUIRE(listed_state() == "streaming");
  }

  SECTION("a resume replaces the session, and ending the superseded stream drains only the old instance") {
    // What /resume does: a new StreamSession, with its own stream state, under the same id
    running_sessions->update([](const immer::vector<events::StreamSession> &sessions) {
      auto resumed = sessions.front();
      resumed.stream_state = std::make_shared<immer::atom<events::StreamState>>();
      return state::remove_session(sessions, sessions.front()).push_back(resumed);
    });
    REQUIRE(listed_state() == "setup"); // even though the old instance is still streaming

    setup(2);
    ping_until_started(2, 4);
    REQUIRE(listed_state() == "streaming");

    event_bus->fire_event(immer::box<events::EndSupersededStreamEvent>(
        events::EndSupersededStreamEvent{.session_id = session_id, .rtp_secret_payload = rtp_secret(1)}));
    std::this_thread::sleep_for(teardown + 300ms);
    REQUIRE(*fake.ended == 2); // only the old instance's
    REQUIRE(listed_state() == "streaming");
  }

  // End the remaining pipelines
  event_bus->fire_event(immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session_id}));
  std::this_thread::sleep_for(teardown + 300ms);
  REQUIRE(*fake.ended == *fake.started);
  REQUIRE(listed_state() == "setup");
  // Every pipeline counted itself out, which the listed state alone can't tell once either count is 0
  auto stream_state = running_sessions->load()->front().stream_state->load();
  REQUIRE(stream_state->video_pipelines == 0);
  REQUIRE(stream_state->audio_pipelines == 0);
}

TEST_CASE("utils::to_size_t", "[API]") {
  REQUIRE(utils::to_size_t("0") == 0);
  REQUIRE(utils::to_size_t("10594003729173467913") == 10594003729173467913ULL);
  REQUIRE(utils::to_size_t("18446744073709551615") == 18446744073709551615ULL);
  REQUIRE_FALSE(utils::to_size_t(""));
  REQUIRE_FALSE(utils::to_size_t("abc"));
  REQUIRE_FALSE(utils::to_size_t("123abc"));
  REQUIRE_FALSE(utils::to_size_t(" 123"));
  REQUIRE_FALSE(utils::to_size_t("-1"));
  REQUIRE_FALSE(utils::to_size_t("18446744073709551616")); // out of range
}

TEST_CASE("Malformed IDs are rejected without crashing", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto socket_path = api_socket_path();
  start_api_server(app_state);

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);
  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  for (const std::string bad_id : {"not-a-number", "", "-1", "99999999999999999999999"}) {
    auto session = rfl::Reflector<wolf::core::events::StreamSession>::ReflType{
        .client_ip = "127.0.0.1",
        .aes_key = "00112233445566778899aabbccddeeff",
        .aes_iv = "1",
        .video_width = 1920,
        .video_height = 1080,
        .video_refresh_rate = 60,
        .app_id = "304556286", // test cfg file
        .client_id = bad_id,
    };
    auto response =
        req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
    REQUIRE(response);
    REQUIRE(response->first == 400);
    REQUIRE_THAT(response->second, ContainsSubstring("Invalid client_id"));

    response = req(curl.get(),
                   HTTPMethod::POST,
                   "http://localhost/api/v1/clients/settings",
                   rfl::json::write(UpdateClientSettingsRequest{.client_id = bad_id}));
    REQUIRE(response);
    REQUIRE(response->first == 400);

    response = req(curl.get(),
                   HTTPMethod::POST,
                   "http://localhost/api/v1/unpair/client",
                   rfl::json::write(UnpairClientRequest{.client_id = bad_id}));
    REQUIRE(response);
    REQUIRE(response->first == 400);

    auto expect_invalid_session = [&](std::string_view endpoint, const std::string &body) {
      auto res = req(curl.get(), HTTPMethod::POST, std::string("http://localhost") + endpoint.data(), body);
      REQUIRE(res);
      REQUIRE(res->first == 400);
      REQUIRE_THAT(res->second, ContainsSubstring("Invalid session_id"));
    };
    expect_invalid_session("/api/v1/sessions/start", rfl::json::write(StreamSessionStartRequest{.session_id = bad_id}));
    expect_invalid_session("/api/v1/sessions/pause", rfl::json::write(StreamSessionPauseRequest{.session_id = bad_id}));
    expect_invalid_session("/api/v1/sessions/stop", rfl::json::write(StreamSessionStopRequest{.session_id = bad_id}));
    expect_invalid_session(
        "/api/v1/sessions/input",
        rfl::json::write(StreamSessionHandleInputRequest{.session_id = bad_id, .input_packet_hex = "00"}));
    expect_invalid_session("/api/v1/runners/start",
                           rfl::json::write(RunnerStartRequest{.stop_stream_when_over = false,
                                                               .runner = wolf::config::AppCMD{.run_cmd = "true"},
                                                               .session_id = bad_id}));
  }

  { // A numeric but unknown session_id is the client's error (it used to dereference an empty optional)
    auto expect_unknown_session = [&](std::string_view endpoint, const std::string &body) {
      auto res = req(curl.get(), HTTPMethod::POST, std::string("http://localhost") + endpoint.data(), body);
      REQUIRE(res);
      REQUIRE(res->first == 404);
      REQUIRE_THAT(res->second, ContainsSubstring("Invalid session_id"));
    };
    expect_unknown_session("/api/v1/sessions/start",
                           rfl::json::write(StreamSessionStartRequest{.session_id = "12345"}));
    expect_unknown_session("/api/v1/sessions/pause",
                           rfl::json::write(StreamSessionPauseRequest{.session_id = "12345"}));
    expect_unknown_session("/api/v1/sessions/stop", rfl::json::write(StreamSessionStopRequest{.session_id = "12345"}));
    expect_unknown_session(
        "/api/v1/sessions/input",
        rfl::json::write(StreamSessionHandleInputRequest{.session_id = "12345", .input_packet_hex = "00"}));
    expect_unknown_session("/api/v1/runners/start",
                           rfl::json::write(RunnerStartRequest{.stop_stream_when_over = false,
                                                               .runner = wolf::config::AppCMD{.run_cmd = "true"},
                                                               .session_id = "12345"}));
  }

  { // An aes_iv that isn't Moonlight's decimal rikeyid would silently encrypt the audio with IV 0
    auto session = rfl::Reflector<wolf::core::events::StreamSession>::ReflType{
        .client_ip = "127.0.0.1",
        .aes_key = "00112233445566778899aabbccddeeff",
        .aes_iv = "abc",
        .video_width = 1920,
        .video_height = 1080,
        .video_refresh_rate = 60,
    };
    auto response =
        req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
    REQUIRE(response);
    REQUIRE(response->first == 400);
    REQUIRE_THAT(response->second, ContainsSubstring("Invalid aes_iv"));
    session.aes_iv = "";
    response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
    REQUIRE(response);
    REQUIRE(response->first == 400);

    // A missing or short key crashed the audio payloader once a stream started
    session.aes_iv = "1";
    for (auto key : {"", "0011", "00112233445566778899aabbccddeefg", "00112233445566778899aabbccddeeff00"}) {
      session.aes_key = key;
      response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
      REQUIRE(response);
      REQUIRE(response->first == 400);
      REQUIRE_THAT(response->second, ContainsSubstring("Invalid aes_key"));
    }
  }

  { // Concurrent adds for one client: exactly one gets the session, the others 409
    auto session = rfl::Reflector<wolf::core::events::StreamSession>::ReflType{
        .client_ip = "127.0.0.1",
        .aes_key = "00112233445566778899aabbccddeeff",
        .aes_iv = "1",
        .video_width = 1920,
        .video_height = 1080,
        .video_refresh_rate = 60,
        .app_id = "304556286",               // test cfg file
        .client_id = "10594003729173467913", // test cfg file
    };
    constexpr int n_clients = 16;
    std::atomic_int created_count = 0, conflicts = 0;
    std::vector<std::thread> clients;
    for (int i = 0; i < n_clients; i++) {
      clients.emplace_back([&]() {
        auto c = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);
        curl_easy_setopt(c.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
        curl_easy_setopt(c.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
        auto res = req(c.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
        if (res && res->first == 200) {
          created_count++;
        } else if (res && res->first == 409) {
          conflicts++;
        }
      });
    }
    for (auto &c : clients) {
      c.join();
    }
    REQUIRE(created_count == 1);
    REQUIRE(conflicts == n_clients - 1);
    REQUIRE(running_sessions->load()->size() == 1);
    running_sessions->store({}); // No handlers here to remove it on sessions/stop
  }

  StreamSessionCreated created;

  { // sessions/start on a real session rejects a zero or absurd display mode instead of streaming it
    auto session = rfl::Reflector<wolf::core::events::StreamSession>::ReflType{
        .client_ip = "127.0.0.1",
        .aes_key = "00112233445566778899aabbccddeeff",
        .aes_iv = "1",
        .video_width = 1920,
        .video_height = 1080,
        .video_refresh_rate = 60,
        .app_id = "304556286",               // test cfg file
        .client_id = "10594003729173467913", // test cfg file
    };
    auto response =
        req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
    REQUIRE(response);
    created = rfl::json::read<StreamSessionCreated>(response->second).value();

    // A second session for the same client would share its session_id, which every session event is keyed by
    response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
    REQUIRE(response);
    REQUIRE(response->first == 409);
    response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
    REQUIRE(response);
    REQUIRE(rfl::json::read<StreamSessionListResponse>(response->second).value().sessions.size() == 1);

    auto fired = std::make_shared<std::atomic<int>>(0);
    auto video_handler = event_bus->register_handler<immer::box<events::VideoSession>>(
        [fired](const immer::box<events::VideoSession> &) { (*fired)++; });
    auto audio_handler = event_bus->register_handler<immer::box<events::AudioSession>>(
        [fired](const immer::box<events::AudioSession> &) { (*fired)++; });
    for (auto mode : {wolf::core::virtual_display::DisplayMode{0, 0, 0},
                      wolf::core::virtual_display::DisplayMode{2147483647, 2147483647, 2147483647},
                      wolf::core::virtual_display::DisplayMode{1920, 1080, 1001}}) {
      auto start_request = StreamSessionStartRequest{.session_id = created.session_id};
      start_request.video_session.display_mode = mode;
      response =
          req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/start", rfl::json::write(start_request));
      REQUIRE(response);
      REQUIRE(response->first == 400);
      REQUIRE_THAT(response->second, ContainsSubstring("Invalid video_session.display_mode"));
    }
    // An encrypted audio session needs a usable key: an empty one crashed the payloader (hex_to_str(NULL))
    for (auto key : {"", "abc"}) {
      auto start_request = StreamSessionStartRequest{.session_id = created.session_id};
      start_request.video_session.display_mode = {1920, 1080, 60};
      start_request.audio_session.encrypt_audio = true;
      start_request.audio_session.aes_key = key;
      start_request.audio_session.aes_iv = "1";
      response =
          req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/start", rfl::json::write(start_request));
      REQUIRE(response);
      REQUIRE(response->first == 400);
      REQUIRE_THAT(response->second, ContainsSubstring("Invalid audio_session.aes_key"));
    }
    REQUIRE(*fired == 0); // rejected before anything reached the event bus
    video_handler.unregister();
    audio_handler.unregister();
  }

  { // runners/start on a real session replies (it used to close the connection without an answer)
    auto response = req(curl.get(),
                        HTTPMethod::POST,
                        "http://localhost/api/v1/runners/start",
                        rfl::json::write(RunnerStartRequest{.stop_stream_when_over = false,
                                                            .runner = wolf::config::AppCMD{.run_cmd = "true"},
                                                            .session_id = created.session_id}));
    REQUIRE(response);
    REQUIRE(response->first == 200);
    REQUIRE_THAT(response->second, Equals("{\"success\":true}"));
  }

  // A non-numeric Content-Length used to abort the whole server
  auto response =
      req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", "{}", {"Content-Length: abc"});
  REQUIRE(response);
  REQUIRE(response->first == 400);

  { // A body that doesn't parse is the client's error, e.g. a client_id sent as a number
    auto response = req(curl.get(),
                        HTTPMethod::POST,
                        "http://localhost/api/v1/sessions/add",
                        "{\"client_ip\":\"127.0.0.1\",\"client_id\":42,\"video_width\":1920,\"video_height\":1080,"
                        "\"video_refresh_rate\":60}");
    REQUIRE(response);
    REQUIRE(response->first == 400);
    response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/lobbies/join", "{\"lobby_id\":1}");
    REQUIRE(response);
    REQUIRE(response->first == 400);
    response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/docker/images/pull", "{}");
    REQUIRE(response);
    REQUIRE(response->first == 400);
    response =
        req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/docker/images/pull", "{\"image_name\":\" \"}");
    REQUIRE(response);
    REQUIRE(response->first == 400);
  }

  // An unknown endpoint gets its 404 (the socket used to be closed with the reply still in flight)
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/no-such-endpoint");
  REQUIRE(response);
  REQUIRE(response->first == 404);

  // The server is still alive
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  REQUIRE(response);
  REQUIRE(response->first == 200);
}

TEST_CASE("Runners: many concurrent starts on one session that exit at once", "[API]") {
  using namespace std::chrono_literals;
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .host = immer::box<state::Host>(state::Host{}),
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  // No compositor here (start_virtual_compositor = false), so runners don't wait for a Wayland socket.
  // Don't setenv() to skip that wait: it races the threads of earlier tests' servers reading the environment.
  auto handlers = sessions::setup_moonlight_handlers(app_state, api_runtime_dir(), {});
  start_api_server(app_state);
  auto socket_path = api_socket_path();

  auto app = std::make_shared<events::App>(
      events::App{.base = {.title = "true", .id = "true", .support_hdr = false},
                  .start_virtual_compositor = false,
                  .start_audio_server = false,
                  .runner = state::get_runner(wolf::config::AppCMD{.run_cmd = "true"}, event_bus)});

  // Each runner exits at once with stop_stream_when_over: the first exit removes the session (and its devices queue)
  // while the other starts are still being handled (this aborted Wolf on a freed devices queue).
  constexpr int rounds = 20;
  constexpr int runners_per_round = 32;
  for (int round = 0; round < rounds; round++) {
    auto session = events::StreamSession{.event_bus = event_bus,
                                         .app = app,
                                         .enet_secret_payload = static_cast<uint32_t>(round + 1),
                                         .session_id = static_cast<std::size_t>(4242 + round)};
    running_sessions->update([&](const auto &sessions) { return sessions.push_back(session); });
    event_bus->fire_event(immer::box<events::StreamSession>(session));

    std::vector<std::thread> clients;
    std::atomic_int answered = 0;
    for (int i = 0; i < runners_per_round; i++) {
      clients.emplace_back([&]() {
        auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);
        curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
        auto response = req(curl.get(),
                            HTTPMethod::POST,
                            "http://localhost/api/v1/runners/start",
                            rfl::json::write(RunnerStartRequest{.stop_stream_when_over = true,
                                                                .runner = wolf::config::AppCMD{.run_cmd = "true"},
                                                                .session_id = std::to_string(session.session_id)}));
        // Either started, or rejected because an earlier runner already ended the session
        if (response && (response->first == 200 || response->first == 404)) {
          answered++;
        }
      });
    }
    for (auto &client : clients) {
      client.join();
    }
    REQUIRE(answered == runners_per_round);

    // Let the runners started this round exit and stop the session before the next one
    for (int i = 0; i < 100 && state::get_session_by_id(*running_sessions->load(), session.session_id); i++) {
      std::this_thread::sleep_for(20ms);
    }
    REQUIRE_FALSE(state::get_session_by_id(*running_sessions->load(), session.session_id));
    std::this_thread::sleep_for(200ms);
  }
}

/**
 * Whether a process runs `<binary> <arg>` (e.g. "sleep 30.123"); if so, it's killed so the test doesn't leak it.
 */
static bool app_left_running(const std::string &cmd) {
  bool found = false;
  for (const auto &proc : std::filesystem::directory_iterator("/proc")) {
    std::ifstream cmdline(proc.path() / "cmdline");
    std::string arg0, arg1;
    if (std::getline(cmdline, arg0, '\0') && std::getline(cmdline, arg1, '\0') &&
        fmt::format("{} {}", std::filesystem::path(arg0).filename().string(), arg1) == cmd) {
      found = true;
    }
  }
  if (found) {
    std::system(fmt::format("pkill -f '^{}$'", cmd).c_str());
  }
  return found;
}

TEST_CASE("Runners: a session stopped while its runner starts doesn't leave the app running", "[API]") {
  using namespace std::chrono_literals;
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .host = immer::box<state::Host>(state::Host{}),
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});
  auto handlers = sessions::setup_moonlight_handlers(app_state, api_runtime_dir(), {});

  // A command line unique to this run, to find the app's process by
  auto app_cmd = fmt::format("sleep 30.{}", getpid());
  auto app = std::make_shared<events::App>(
      events::App{.base = {.title = "sleep", .id = "sleep", .support_hdr = false},
                  .start_virtual_compositor = false,
                  .start_audio_server = false,
                  .runner = state::get_runner(wolf::config::AppCMD{.run_cmd = app_cmd}, event_bus)});
  auto session = events::StreamSession{.event_bus = event_bus, .app = app, .session_id = 4343};
  running_sessions->update([&](const auto &sessions) { return sessions.push_back(session); });
  event_bus->fire_event(immer::box<events::StreamSession>(session)); // starts the app's runner on another thread

  // The stop lands before the runner has registered its stop handlers (it does so only once the app is spawned), as
  // when an API client adds and stops a session right away: only the session being gone tells the runner.
  running_sessions->update([&](const auto &sessions) { return state::remove_session(sessions, session); });

  // The runner spawns the app, then sees the session is gone and ends it (it used to run on, an orphan)
  std::this_thread::sleep_for(2s);
  REQUIRE_FALSE(app_left_running(app_cmd));
  std::this_thread::sleep_for(200ms); // let the runner thread finish before the bus goes away
}

TEST_CASE("Runners: a lobby stopped while its runner starts doesn't leave the app running", "[API]") {
  using namespace std::chrono_literals;
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .lobbies = lobbies,
      .running_sessions = running_sessions});
  // gst-wayland-display creates the desktop's socket in $XDG_RUNTIME_DIR: wait for it there, so that the test also
  // passes on its own, without CTest's WOLF_SKIP_WAYLAND_SOCKET_WAIT
  auto handlers = sessions::setup_lobbies_handlers(app_state, utils::get_env("XDG_RUNTIME_DIR", "/tmp"), {});

  auto app_cmd = fmt::format("sleep 31.{}", getpid());
  auto lobby_id = fmt::format("lobby-{}", getpid());
  auto create = events::CreateLobbyEvent{
      .id = lobby_id,
      .profile_id = "test_profile",
      .name = "test_lobby",
      .multi_user = false,
      .stop_when_everyone_leaves = false,
      .video_settings = {.width = 640,
                         .height = 480,
                         .refresh_rate = 30,
                         .wayland_render_node = "software",
                         .runner_render_node = "runner_render_node",
                         .video_producer_buffer_caps = "video/x-raw"},
      .audio_settings = {.channel_count = 2},
      .runner_state_folder = "runner_state_folder",
      .runner = state::get_runner(wolf::config::AppCMD{.run_cmd = app_cmd}, event_bus)};
  auto setup_over = create.on_setup_over.get()->get_future();
  event_bus->fire_event(immer::box<events::CreateLobbyEvent>(create)); // the runner starts once the desktop is up

  // Gone before its runner registered its stop handlers: only the lobby being gone tells the runner
  lobbies->update([&](const immer::vector<events::Lobby> &all) {
    immer::vector<events::Lobby> kept;
    for (const auto &lobby : all) {
      if (lobby.id != lobby_id) {
        kept = kept.push_back(lobby);
      }
    }
    return kept;
  });

  // Needs gst-wayland-display (waylanddisplaysrc) on GST_PLUGIN_PATH, like "Lobbies APIs"
  REQUIRE(setup_over.wait_for(20s) == std::future_status::ready);
  std::this_thread::sleep_for(2s);
  auto orphan = app_left_running(app_cmd);
  // Ends the lobby's desktop and audio producers, which listen for its stop
  event_bus->fire_event(immer::box<events::StopLobbyEvent>(events::StopLobbyEvent{.lobby_id = lobby_id}));
  std::this_thread::sleep_for(500ms);
  REQUIRE_FALSE(orphan);
}

TEST_CASE("Session APIs without app_id or client_id", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  start_api_server(app_state);

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  // Test that the initial list of sessions is empty
  auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  REQUIRE(response);
  auto sessions = rfl::json::read<StreamSessionListResponse>(response->second).value();
  REQUIRE(sessions.success);
  REQUIRE(sessions.sessions.size() == 0);

  // Test that we can add a session
  auto session = rfl::Reflector<wolf::core::events::StreamSession>::ReflType{
      .client_ip = "127.0.0.1",
      .aes_key = "00112233445566778899aabbccddeeff",
      .aes_iv = "1",
      .video_width = 1920,
      .video_height = 1080,
      .video_refresh_rate = 60,
  };

  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
  REQUIRE(response);
  REQUIRE_THAT(response->second, Catch::Matchers::ContainsSubstring("{\"success\":true,\"session_id\":"));
  auto first = rfl::json::read<StreamSessionCreated>(response->second).value();

  // Test that the new session is in the list
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  REQUIRE(response);
  auto sessions2 = rfl::json::read<StreamSessionListResponse>(response->second).value();
  REQUIRE(sessions2.success);
  REQUIRE(sessions2.sessions.size() == 1);

  // Each such session gets its own session_id (they all used to share the hash of an empty cert)
  response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/sessions/add", rfl::json::write(session));
  REQUIRE(response);
  REQUIRE(response->first == 200);
  auto second = rfl::json::read<StreamSessionCreated>(response->second).value();
  REQUIRE(first.session_id != second.session_id);
  response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/sessions");
  REQUIRE(response);
  REQUIRE(rfl::json::read<StreamSessionListResponse>(response->second).value().sessions.size() == 2);
}

// A device unplugged while its session is inside a lobby must be relayed to the lobby's runner.
TEST_CASE("Lobby relays a device unplug to its runner", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
      .running_sessions = running_sessions});

  // A lobby with one connected Moonlight session, added straight to state (no compositor).
  const std::string lobby_id = "lobby-under-test";
  const std::string moonlight_session_id = "1234";
  events::Lobby lobby{.id = lobby_id,
                      .name = "test_lobby",
                      .started_by_profile_id = "test_profile",
                      .multi_user = false,
                      .stop_when_everyone_leaves = false};
  lobby.connected_sessions->update([&](const immer::vector<immer::box<std::string>> &connected) {
    return connected.push_back(immer::box<std::string>{moonlight_session_id});
  });
  app_state->lobbies->update([&](const immer::vector<events::Lobby> &lobbies) { return lobbies.push_back(lobby); });

  auto lobbies_handlers = sessions::setup_lobbies_handlers(app_state, api_runtime_dir(), {});

  int relayed_to_lobby = 0;
  auto observer = event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
      [&](const immer::box<events::UnplugDeviceEvent> &ev) {
        if (ev->session_id == lobby_id) {
          relayed_to_lobby++;
        }
      });

  event_bus->fire_event(
      immer::box<events::UnplugDeviceEvent>{events::UnplugDeviceEvent{.session_id = moonlight_session_id}});

  REQUIRE(relayed_to_lobby == 1);
}

TEST_CASE("Joining a second lobby leaves the first one", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
      .running_sessions = running_sessions});

  // Two lobbies and a session, added straight to state (no compositor)
  for (auto id : {"lobby-a", "lobby-b"}) {
    events::Lobby lobby{.id = id, .name = id, .multi_user = false, .stop_when_everyone_leaves = false};
    app_state->lobbies->update([&](const immer::vector<events::Lobby> &lobbies) { return lobbies.push_back(lobby); });
  }
  auto session = events::StreamSession{.event_bus = event_bus, .session_id = 4321};
  running_sessions->update([&](const auto &sessions) { return sessions.push_back(session); });
  auto lobbies_handlers = sessions::setup_lobbies_handlers(app_state, api_runtime_dir(), {});

  std::vector<std::string> switched_to;
  auto observer = event_bus->register_handler<immer::box<events::SwitchStreamProducerEvents>>(
      [&](const immer::box<events::SwitchStreamProducerEvents> &ev) { switched_to.push_back(ev->interpipe_src_id); });
  auto join = [&](const std::string &lobby_id) {
    auto ev = events::JoinLobbyEvent{.lobby_id = lobby_id, .moonlight_session_id = session.session_id};
    event_bus->fire_event(immer::box<events::JoinLobbyEvent>(ev));
    return ev.error_message.get()->get_future().get();
  };
  auto members = [&](const std::string &lobby_id) {
    return state::get_lobby_by_id(*app_state->lobbies->load(), lobby_id)->connected_sessions->load()->size();
  };

  REQUIRE(join("lobby-a").empty());
  // A stream starting now resolves its producers to the first lobby listing the session: it must be only B
  REQUIRE(join("lobby-b").empty());
  REQUIRE(members("lobby-a") == 0);
  REQUIRE(members("lobby-b") == 1);
  REQUIRE(switched_to.back() == "lobby-b");
  // Joining the lobby it's already in changes nothing
  REQUIRE(join("lobby-b").empty());
  REQUIRE(members("lobby-b") == 1);

  // Concurrent joins to both: the session ends up in exactly one lobby, listed once
  for (int round = 0; round < 20; round++) {
    std::thread to_a([&]() { join("lobby-a"); });
    std::thread to_b([&]() { join("lobby-b"); });
    to_a.join();
    to_b.join();
    REQUIRE(members("lobby-a") + members("lobby-b") == 1);
  }
}

TEST_CASE("Lobbies APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
      .running_sessions = running_sessions});
  auto socket_path = api_socket_path();
  // Setup the event bus handlers for the lobbies events.
  // gst-wayland-display creates the desktop's socket in $XDG_RUNTIME_DIR: wait for it there. This passed with
  // api_runtime_dir() only while process runners leaked their XDG_RUNTIME_DIR into the test's own environment.
  auto lobbies_handlers = sessions::setup_lobbies_handlers(app_state, utils::get_env("XDG_RUNTIME_DIR", "/tmp"), {});
  start_api_server(app_state);

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);
  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  { // Test that the initial list of lobbies is empty
    auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    auto sessions = rfl::json::read<LobbiesResponse>(response->second).value();
    REQUIRE(sessions.success);
    REQUIRE(sessions.lobbies.size() == 0);
  }

  { // A zero or absurd display mode is rejected before any video producer is started
    auto created = std::make_shared<std::atomic<int>>(0);
    auto create_handler = event_bus->register_handler<immer::box<events::CreateLobbyEvent>>(
        [created](const immer::box<events::CreateLobbyEvent> &) { (*created)++; });
    for (auto mode : {wolf::core::virtual_display::DisplayMode{0, 0, 0},
                      wolf::core::virtual_display::DisplayMode{2147483647, 2147483647, 2147483647},
                      wolf::core::virtual_display::DisplayMode{1920, 1080, 1001}}) {
      auto bad_lobby = CreateLobbyRequest{.profile_id = "test_profile",
                                          .name = "bad_lobby",
                                          .multi_user = false,
                                          .stop_when_everyone_leaves = false,
                                          .video_settings = {.width = mode.width,
                                                             .height = mode.height,
                                                             .refresh_rate = mode.refreshRate,
                                                             .wayland_render_node = "software",
                                                             .runner_render_node = "runner_render_node",
                                                             .video_producer_buffer_caps = "video/x-raw"},
                                          .audio_settings = {.channel_count = 2},
                                          .runner_state_folder = "runner_state_folder",
                                          .runner = wolf::config::AppCMD{.run_cmd = "sleep 10"}};
      auto response =
          req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/lobbies/create", rfl::json::write(bad_lobby));
      REQUIRE(response);
      REQUIRE(response->first == 400);
      REQUIRE_THAT(response->second, ContainsSubstring("Invalid video_settings"));
    }
    REQUIRE(*created == 0);
    create_handler.unregister();
    auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    REQUIRE(rfl::json::read<LobbiesResponse>(response->second).value().lobbies.empty());
  }

  std::string lobby_id;
  std::vector<short> pin = {1, 2, 3, 4};
  { // Test creating a lobby
    auto new_lobby = CreateLobbyRequest{.profile_id = "test_profile",
                                        .name = "test_lobby",
                                        .multi_user = false,
                                        .pin = pin,
                                        .stop_when_everyone_leaves = false,
                                        .video_settings = {.width = 1920,
                                                           .height = 1080,
                                                           .refresh_rate = 60,
                                                           .wayland_render_node = "software",
                                                           .runner_render_node = "runner_render_node",
                                                           .video_producer_buffer_caps = "video/x-raw"},
                                        .audio_settings = {.channel_count = 2},
                                        .runner_state_folder = "runner_state_folder",
                                        .runner = wolf::config::AppCMD{.run_cmd = "sleep 10"}};
    auto response =
        req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/lobbies/create", rfl::json::write(new_lobby));
    REQUIRE(response);
    INFO("lobbies/create replied: " << response->second);
    REQUIRE(response->first == 200);
    auto lobby_res = rfl::json::read<LobbyCreateResponse>(response->second).value();
    REQUIRE(lobby_res.success);
    REQUIRE(!lobby_res.lobby_id.empty());
    lobby_id = lobby_res.lobby_id;
    // Test that the lobby is listed in the list of lobbies
    response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    auto sessions = rfl::json::read<LobbiesResponse>(response->second).value();
    REQUIRE(sessions.success);
    REQUIRE(sessions.lobbies.size() == 1);
    REQUIRE_THAT(sessions.lobbies[0].id, Equals(lobby_res.lobby_id));
    REQUIRE(sessions.lobbies[0].connected_sessions.size() == 0);
  }

  std::size_t moonlight_session_id = 1234;
  { // Test joining a lobby
    // First, create a StreamSession
    running_sessions->update([moonlight_session_id](const immer::vector<events::StreamSession> &sessions) {
      return sessions.push_back(events::StreamSession{.session_id = moonlight_session_id});
    });
    // Then, call the join endpoint
    auto payload =
        rfl::json::write(events::JoinLobbyEvent{.lobby_id = lobby_id, .moonlight_session_id = moonlight_session_id});
    auto response = req(curl.get(), HTTPMethod::POST, "http://localhost/api/v1/lobbies/join", payload);
    REQUIRE(response);
    auto error_res = rfl::json::read<GenericErrorResponse>(response->second).value();
    // we expect this to fail since we don't pass the PIN!
    REQUIRE(!error_res.success);
    REQUIRE(response->first == 403);
    // An unknown lobby is the client's error too
    response = req(
        curl.get(),
        HTTPMethod::POST,
        "http://localhost/api/v1/lobbies/join",
        rfl::json::write(events::JoinLobbyEvent{.lobby_id = "nope", .moonlight_session_id = moonlight_session_id}));
    REQUIRE(response);
    REQUIRE(response->first == 404);

    // Now call it with the right PIN
    response = req(
        curl.get(),
        HTTPMethod::POST,
        "http://localhost/api/v1/lobbies/join",
        rfl::json::write(
            events::JoinLobbyEvent{.lobby_id = lobby_id, .moonlight_session_id = moonlight_session_id, .pin = pin}));
    REQUIRE(response);
    auto join_res = rfl::json::read<GenericSuccessResponse>(response->second).value();
    REQUIRE(join_res.success);

    // Test that the lobby now lists this session
    response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    auto sessions = rfl::json::read<LobbiesResponse>(response->second).value();
    REQUIRE(sessions.success);
    REQUIRE(sessions.lobbies.size() == 1);
    REQUIRE_THAT(sessions.lobbies[0].id, Equals(lobby_id));
    REQUIRE(sessions.lobbies[0].connected_sessions.size() == 1);
    REQUIRE_THAT(sessions.lobbies[0].connected_sessions[0], Equals(std::to_string(moonlight_session_id)));
  }

  { // Test leaving a lobby
    auto response = req(
        curl.get(),
        HTTPMethod::POST,
        "http://localhost/api/v1/lobbies/leave",
        rfl::json::write(events::LeaveLobbyEvent{.lobby_id = lobby_id, .moonlight_session_id = moonlight_session_id}));
    REQUIRE(response);
    auto leave_res = rfl::json::read<GenericSuccessResponse>(response->second).value();
    REQUIRE(leave_res.success);
    // Test that the lobby no longer lists this session
    response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    auto sessions = rfl::json::read<LobbiesResponse>(response->second).value();
    REQUIRE(sessions.success);
    REQUIRE(sessions.lobbies.size() == 1);
    REQUIRE_THAT(sessions.lobbies[0].id, Equals(lobby_id));
    REQUIRE(sessions.lobbies[0].connected_sessions.empty());
  }

  { // Test stopping the lobby
    auto response = req(curl.get(),
                        HTTPMethod::POST,
                        "http://localhost/api/v1/lobbies/stop",
                        rfl::json::write(events::StopLobbyEvent{.lobby_id = lobby_id}));
    REQUIRE(response);
    auto error_res = rfl::json::read<GenericSuccessResponse>(response->second).value();
    // We expect this to fail since we don't pass a PIN!
    REQUIRE(!error_res.success);
    REQUIRE(response->first == 403);
    // Now try again with the PIN
    response = req(curl.get(),
                   HTTPMethod::POST,
                   "http://localhost/api/v1/lobbies/stop",
                   rfl::json::write(events::StopLobbyEvent{.lobby_id = lobby_id, .pin = pin}));
    REQUIRE(response);
    auto stop_res = rfl::json::read<GenericSuccessResponse>(response->second).value();
    REQUIRE(stop_res.success);

    // Test that the lobby no longer exists
    response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/lobbies");
    REQUIRE(response);
    auto sessions = rfl::json::read<LobbiesResponse>(response->second).value();
    REQUIRE(sessions.success);
    REQUIRE(sessions.lobbies.empty());
  }

  // Give it a few seconds to turn down the Gstreamer pipeline
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

TEST_CASE("Utils APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
      .running_sessions = running_sessions});
  auto socket_path = api_socket_path();
  start_api_server(app_state);

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);
  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  { // Test that calling
    // http://localhost/api/v1/utils/get-icon\?icon_path\=https://games-on-whales.github.io/wildlife/apps/firefox/assets/icon.png
    // returns the content of the PNG file
    auto response = req(curl.get(),
                        HTTPMethod::GET,
                        "http://localhost/api/v1/utils/get-icon?icon_path=https://games-on-whales.github.io/wildlife/"
                        "apps/firefox/assets/icon.png");

    REQUIRE(response);
    REQUIRE_THAT(response->second, Catch::Matchers::ContainsSubstring("PNG"));
  }

  { // Test that when getting a non 200 from a remote resource we don't return an icon
    auto response = req(curl.get(),
                        HTTPMethod::GET,
                        "http://localhost/api/v1/utils/get-icon?icon_path=https://games-on-whales.github.io/wildlife/"
                        "THIS_PAGE_DOES_NOT_EXISTS");

    REQUIRE(response);
    REQUIRE(response->first == 404);
  }

  { // A remote resource that isn't an image (an HTML page) is not served either
    auto response = req(curl.get(),
                        HTTPMethod::GET,
                        "http://localhost/api/v1/utils/get-icon?icon_path=https://games-on-whales.github.io/wildlife/");

    REQUIRE(response);
    REQUIRE(response->first == 404);
  }

  // Local files: only images are served, anything else (keys, config, /etc/shadow, ...) is refused
  // One dir per process: concurrent runs on a host must not share it
  auto icons_dir = std::filesystem::temp_directory_path() / ("wolf-test-icons-" + std::to_string(::getpid()));
  std::filesystem::remove_all(icons_dir); // Leftovers of a previous failed run (mkfifo below needs a clean dir)
  std::filesystem::create_directories(icons_dir);
  auto png_path = icons_dir / "icon.png";
  auto key_path = icons_dir / "key.png"; // Extension must not matter, only the content
  std::ofstream(png_path, std::ios::binary) << std::string("\x89PNG\r\n\x1a\n-fake-png-body", 22);
  std::ofstream(key_path, std::ios::binary) << "-----BEGIN PRIVATE KEY-----\nsecret\n-----END PRIVATE KEY-----\n";
  auto svg_path = icons_dir / "icon.svg";
  std::ofstream(svg_path) << R"(<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"/>)";

  auto big_png_path = icons_dir / "big.png"; // Over the 10 MiB limit
  std::ofstream(big_png_path, std::ios::binary)
      << std::string("\x89PNG\r\n\x1a\n", 8) << std::string(10 * 1024 * 1024, 'x');

  auto fifo_path = icons_dir / "fifo.png";
  REQUIRE(::mkfifo(fifo_path.c_str(), 0600) == 0);

  // Directories, devices, FIFOs and oversized files are refused, and must not take the server down (it has to answer
  // the next requests)
  for (const auto &path : {icons_dir.string(), std::string("/dev/zero"), fifo_path.string(), big_png_path.string()}) {
    auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/utils/get-icon?icon_path=" + path);
    REQUIRE(response);
    REQUIRE(response->first == 404);
  }

  { // A local PNG is served, with its detected Content-Type
    auto response =
        req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/utils/get-icon?icon_path=" + png_path.string());
    REQUIRE(response);
    REQUIRE(response->first == 200);
    REQUIRE_THAT(response->second, ContainsSubstring("PNG"));
    char *content_type = nullptr;
    curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_TYPE, &content_type);
    REQUIRE(content_type);
    REQUIRE_THAT(content_type, Equals("image/png"));
  }

  { // Content-Type follows the detected image type
    auto response =
        req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/utils/get-icon?icon_path=" + svg_path.string());
    REQUIRE(response);
    REQUIRE(response->first == 200);
    char *content_type = nullptr;
    curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_TYPE, &content_type);
    REQUIRE(content_type);
    REQUIRE_THAT(content_type, Equals("image/svg+xml"));
  }

  for (const auto &path : {key_path.string(), std::string("/etc/passwd"), std::string("config.test.toml")}) {
    auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/utils/get-icon?icon_path=" + path);
    REQUIRE(response);
    REQUIRE(response->first == 404);
    REQUIRE_THAT(response->second, !ContainsSubstring("PRIVATE KEY") && !ContainsSubstring("root:"));
  }

  std::filesystem::remove_all(icons_dir);
}

TEST_CASE("image_mime_type sniffs content and not extensions", "[API]") {
  using namespace std::string_literals;
  REQUIRE(utils::image_mime_type("\x89PNG\r\n\x1a\nrest"s) == "image/png");
  REQUIRE(utils::image_mime_type("\xff\xd8\xff\xe0rest"s) == "image/jpeg");
  REQUIRE(utils::image_mime_type("GIF89arest"s) == "image/gif");
  REQUIRE(utils::image_mime_type("RIFF\x10\x00\x00\x00WEBPVP8 "s) == "image/webp");
  REQUIRE(utils::image_mime_type("\x00\x00\x01\x00\x01\x00"s) == "image/x-icon");
  REQUIRE(utils::image_mime_type("<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>"s) ==
          "image/svg+xml");
  REQUIRE(utils::image_mime_type("<!DOCTYPE svg [\n  <!ENTITY ns_svg \"http://www.w3.org/2000/svg\">\n] >\n<svg>"s) ==
          "image/svg+xml"); // DOCTYPE with an internal subset
  REQUIRE(utils::image_mime_type("<!DOCTYPE svg SYSTEM \"x[1].dtd\" [<!ENTITY a \"]>\"><!-- ] > -->]><svg/>"s) ==
          "image/svg+xml"); // Brackets and > inside literals and comments
  REQUIRE(utils::image_mime_type("<!DOCTYPE svg [<?pi ' \" ]> ?>]><svg/>"s) == "image/svg+xml"); // ... and PIs
  REQUIRE(utils::image_mime_type("  <svg/>"s) == "image/svg+xml");
  REQUIRE(utils::image_mime_type("\xef\xbb\xbf<svg>"s) == "image/svg+xml"); // UTF-8 BOM
  REQUIRE(utils::image_mime_type("<?xml version=\"1.0\"?><!-- made by hand -->\n<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG "
                                 "1.1//EN\" \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\n<svg>"s) ==
          "image/svg+xml");

  REQUIRE_FALSE(utils::image_mime_type(""s));
  REQUIRE_FALSE(utils::image_mime_type("-----BEGIN PRIVATE KEY-----\n"s));
  REQUIRE_FALSE(utils::image_mime_type("root:$6$hash:19000:0:99999:7:::\n"s));
  REQUIRE_FALSE(utils::image_mime_type("[[apps]]\ntitle = \"<svg>\"\n"s));
  REQUIRE_FALSE(utils::image_mime_type("RIFF\x10\x00\x00\x00WAVE"s));
  REQUIRE_FALSE(utils::image_mime_type("\xff\x00secret"s));    // Not a full JPEG magic
  REQUIRE_FALSE(utils::image_mime_type("<?><svg/>?>SECRET"s)); // Still in the PI
  REQUIRE_FALSE(utils::image_mime_type("<!DOCTYPE config SYSTEM \"a[b\"><config>SECRET</config><!-- ] --><svg/>"s));
  REQUIRE_FALSE(utils::image_mime_type("<!DOCTYPE html PUBLIC \"-//X//[secret\" \"x.dtd\"><html>SECRET ]><svg/>"s));
  REQUIRE_FALSE(utils::image_mime_type("<!DOCTYPE x [<?p ' ?>]><x>SECRET ' ]><svg/></x>"s)); // PI in the subset
  REQUIRE_FALSE(utils::image_mime_type("<!DOCTYPE x [<?p ]><svg/> ?>]><x>SECRET</x>"s));
  REQUIRE_FALSE(utils::image_mime_type("<!DOCTYPE x [<?p ?>]><x>SECRET</x> ]><svg/>"s));    // PI ends at ?>, not >
  REQUIRE_FALSE(utils::image_mime_type("<!DOCTYPE svg SYSTEM \"x.dtd><svg/>"s));            // Unterminated literal
  REQUIRE_FALSE(utils::image_mime_type("<!--><svg/>-->SECRET"s));                           // Still in the comment
  REQUIRE_FALSE(utils::image_mime_type("<!-- <svg > --><secrets>KEY</secrets>"s));          // <svg isn't the root
  REQUIRE_FALSE(utils::image_mime_type("<html><body><svg width=1/>SECRET</body></html>"s)); // <svg isn't the root
  REQUIRE_FALSE(utils::image_mime_type("<svg-not-really> -----BEGIN PRIVATE KEY-----"s));   // Not an <svg> tag
  REQUIRE_FALSE(utils::image_mime_type("<config>" + std::string(1024, ' ') + "<svg/>"));    // Too far from the start
}

struct SSEEvent {
  std::string event;
  std::string data;
};

void listen_sse(CURL *handle, std::shared_ptr<TSQueue<SSEEvent>> queue, std::string_view api_endpoint) {
  curl_easy_setopt(handle, CURLOPT_URL, api_endpoint.data());
  curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  curl_easy_setopt(
      handle,
      CURLOPT_WRITEFUNCTION,
      static_cast<size_t (*)(char *, size_t, size_t, void *)>([](char *ptr, size_t size, size_t nmemb, void *tsqueue) {
        std::string data{ptr, size * nmemb};
        logs::log(logs::debug, "[CURL] Received: {}", data);

        auto ts_queue = static_cast<TSQueue<SSEEvent> *>(tsqueue);
        auto lines = utils::split(data, '\n');
        if (lines.size() >= 2 && lines[0].starts_with("event: ") && lines[1].starts_with("data: ")) {
          ts_queue->push(SSEEvent{.event = std::string(lines[0].substr(7)), .data = std::string(lines[1].substr(6))});
        } else {
          logs::log(logs::warning, "[CURL] Invalid SSE event: {}", data);
        }

        return size * nmemb;
      }));
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, reinterpret_cast<void *>(queue.get()));

  curl_easy_perform(handle);
}

TEST_CASE("SSE APIs", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  // Start the server
  start_api_server(app_state);

  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);

  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());

  auto queue = std::make_shared<TSQueue<SSEEvent>>();

  std::thread sse_thread(listen_sse, curl.get(), queue, "http://localhost/api/v1/events");
  sse_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(42)); // Wait for the SSE client to start

  // Test out one of the events
  // The subscriber is registered asynchronously: an event fired before that reaches nobody, so retry until one lands
  std::optional<SSEEvent> event;
  for (int attempt = 0; attempt < 50 && !event; attempt++) {
    event_bus->fire_event(events::IDRRequestEvent{.session_id = 42});
    event = queue->pop();
  }
  REQUIRE(event.has_value());
  REQUIRE_THAT(event->event, Equals("wolf::core::events::IDRRequestEvent"));
  REQUIRE_THAT(event->data, Equals("{\"session_id\":\"42\"}"));
}

TEST_CASE("SSE APIs large events", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});

  auto runtime_dir = api_runtime_dir();
  auto socket_path = api_socket_path();
  start_api_server(app_state);

  boost::asio::io_context client_ctx;
  boost::asio::local::stream_protocol::socket client(client_ctx);
  client.connect(boost::asio::local::stream_protocol::endpoint(socket_path));
  boost::asio::write(client, boost::asio::buffer(std::string_view("GET /api/v1/events HTTP/1.0\r\n\r\n")));
  boost::asio::streambuf headers;
  boost::asio::read_until(client, headers, "\r\n\r\n");
  std::string received(boost::asio::buffers_begin(headers.data()), boost::asio::buffers_end(headers.data()));
  received = received.substr(received.find("\r\n\r\n") + 4);

  // Each event is bigger than the socket buffer, so the first write is still in flight when the second one is queued
  auto first = events::DockerContainerCreated{.container_id = std::string(1024 * 1024, 'a')};
  auto second = events::DockerContainerCreated{.container_id = std::string(1024 * 1024, 'b')};
  auto as_sse = [](const events::DockerContainerCreated &ev) {
    return fmt::format("event: wolf::core::events::DockerContainerCreated\ndata: {}\n\n", rfl::json::write(ev));
  };
  auto expected = as_sse(first) + as_sse(second);
#ifdef __GLIBC__
  mallopt(M_PERTURB, 165); // scrub freed memory, so an event sent from a freed buffer is reliably corrupted
#endif
  event_bus->fire_event(immer::box<events::DockerContainerCreated>(first));
  event_bus->fire_event(immer::box<events::DockerContainerCreated>(second));

  std::array<char, 64 * 1024> chunk{};
  while (received.size() < expected.size()) {
    received.append(chunk.data(), client.read_some(boost::asio::buffer(chunk)));
    // A keepalive may legitimately land between two events
    for (auto pos = received.find(":keepalive\n\n"); pos != std::string::npos; pos = received.find(":keepalive\n\n")) {
      received.erase(pos, 12);
    }
  }
#ifdef __GLIBC__
  mallopt(M_PERTURB, 0);
#endif
  REQUIRE(received.size() == expected.size());
  // Compare by offset: printing two 2 MiB strings on failure is useless
  auto first_mismatch = std::mismatch(received.begin(), received.end(), expected.begin()).first - received.begin();
  REQUIRE(first_mismatch == static_cast<long>(expected.size()));

  {
    // A subscriber that keeps up gets every event, even when a single one is bigger than MAX_WRITE_QUEUE_BYTES
    auto huge_event = events::DockerContainerCreated{.container_id = std::string(65 * 1024 * 1024, 'h')};
    auto huge_expected = as_sse(huge_event) + as_sse(huge_event);
    std::string huge_received;
    huge_received.reserve(huge_expected.size());
    // Both are queued before we start reading: the first one is still in flight when the second one arrives
    event_bus->fire_event(immer::box<events::DockerContainerCreated>(huge_event));
    event_bus->fire_event(immer::box<events::DockerContainerCreated>(huge_event));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<char> buf(1024 * 1024);
    boost::system::error_code read_ec;
    while (huge_received.size() < huge_expected.size() && !read_ec) {
      huge_received.append(buf.data(), client.read_some(boost::asio::buffer(buf), read_ec));
    }
    for (auto pos = huge_received.find(":keepalive\n\n"); pos != std::string::npos;
         pos = huge_received.find(":keepalive\n\n")) {
      huge_received.erase(pos, 12);
    }
    REQUIRE(huge_received.size() == huge_expected.size());
    REQUIRE(huge_received.compare(huge_expected) == 0);
  }

  // A subscriber that stops reading is dropped instead of having every event buffered for it
  auto big_event = events::DockerContainerCreated{.container_id = std::string(1024 * 1024, 'c')};
  for (int i = 0; i < 80; i++) { // More than MAX_WRITE_QUEUE_BYTES
    event_bus->fire_event(immer::box<events::DockerContainerCreated>(big_event));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Let the server queue them up
  // Fail instead of hanging if the server never drops us (keepalives would keep a blocking read going forever)
  client.non_blocking(true);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  boost::system::error_code ec;
  std::size_t drained = 0;
  while (ec != boost::asio::error::eof && std::chrono::steady_clock::now() < deadline) {
    pollfd fd = {.fd = client.native_handle(), .events = POLLIN, .revents = 0};
    if (poll(&fd, 1, 100) > 0) {
      drained += client.read_some(boost::asio::buffer(chunk), ec);
    }
  }
  REQUIRE(ec == boost::asio::error::eof);
  REQUIRE(drained < 80 * big_event.container_id.size());
}

TEST_CASE("API reports a config change that couldn't be saved", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  const std::string cfg_file = "config.test.UNSAVED.toml";
  std::filesystem::copy_file("config.test.toml", cfg_file, std::filesystem::copy_options::overwrite_existing);
  auto config = immer::box<state::Config>(state::load_or_default(cfg_file, event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});
  auto socket_path = api_socket_path();
  start_api_server(app_state);
  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);
  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  // config.toml emptied under the running Wolf: saving it back would write an empty uuid and hostname
  { std::ofstream(cfg_file, std::ios::trunc); }

  auto expect_not_saved = [&](std::string_view endpoint, const std::string &body) {
    auto response = req(curl.get(), HTTPMethod::POST, std::string("http://localhost") + endpoint.data(), body);
    REQUIRE(response);
    REQUIRE(response->first == 500);
    REQUIRE_THAT(response->second, ContainsSubstring("couldn't be saved"));
    REQUIRE(std::filesystem::file_size(cfg_file) == 0); // left untouched
  };
  auto app = rfl::Reflector<wolf::core::events::App>::ReflType{.title = "Unsaved app",
                                                               .id = "unsaved",
                                                               .runner = wolf::config::AppCMD{.run_cmd = "true"}};
  expect_not_saved("/api/v1/apps/add", rfl::json::write(app));
  expect_not_saved("/api/v1/clients/settings",
                   rfl::json::write(UpdateClientSettingsRequest{.client_id = "10594003729173467913"})); // test cfg file
  expect_not_saved("/api/v1/unpair/client", rfl::json::write(UnpairClientRequest{.client_id = "10594003729173467913"}));
  std::filesystem::remove(cfg_file);
}

TEST_CASE("A failed docker pull is reported as a failure", "[API]") {
  // A fake Docker engine whose pull stream ends in an error, as dockerd reports e.g. a missing image
  auto socket_path = (std::filesystem::path(api_runtime_dir()) / "fake-docker.sock").string();
  std::filesystem::remove(socket_path);
  boost::asio::io_context ctx;
  boost::asio::local::stream_protocol::acceptor acceptor(ctx,
                                                         boost::asio::local::stream_protocol::endpoint(socket_path));
  std::string pull_request;
  std::thread fake_docker([&acceptor, &pull_request]() {
    // DockerAPI asks for /version first, then pulls
    for (auto body : {std::string("{\"ApiVersion\":\"1.41\"}"),
                      std::string("{\"status\":\"Pulling from library/nope\",\"id\":\"latest\"}\r\n"
                                  "{\"errorDetail\":{\"message\":\"boom\"},\"error\":\"boom\"}\r\n")}) {
      boost::asio::local::stream_protocol::socket conn(acceptor.get_executor());
      acceptor.accept(conn);
      boost::asio::streambuf request;
      boost::asio::read_until(conn, request, "\r\n\r\n");
      pull_request = std::string(boost::asio::buffers_begin(request.data()), boost::asio::buffers_end(request.data()));
      auto reply = fmt::format("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\n"
                               "Connection: close\r\n\r\n{}",
                               body.size(),
                               body);
      boost::asio::write(conn, boost::asio::buffer(reply));
    }
  });

  docker::DockerAPI docker_api(socket_path);
  REQUIRE_FALSE(docker_api.pull_image("nope&tag=evil", ""));
  fake_docker.join();
  // The name is escaped: it can't add query parameters
  REQUIRE_THAT(pull_request, ContainsSubstring("/images/create?fromImage=nope%26tag%3Devil%3Alatest "));
  std::filesystem::remove(socket_path);
}

TEST_CASE("The OpenAPI spec in the docs matches the served one", "[API]") {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = immer::box<state::Config>(state::load_or_default("config.test.toml", event_bus, running_sessions));
  auto app_state = immer::box<state::AppState>(state::AppState{
      .config = config,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .running_sessions = running_sessions});
  auto socket_path = api_socket_path();
  start_api_server(app_state);
  auto curl = curl_ptr(curl_easy_init(), ::curl_easy_cleanup);
  curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  auto response = req(curl.get(), HTTPMethod::GET, "http://localhost/api/v1/openapi-schema");
  REQUIRE(response);
  REQUIRE(response->first == 200);
  std::ifstream docs_file("openapi-spec.json"); // docs/modules/dev/partials/spec.json, copied by CMake
  REQUIRE(docs_file.good());
  std::string docs_spec((std::istreambuf_iterator<char>(docs_file)), std::istreambuf_iterator<char>());
  // Compared as JSON values: key order and formatting don't matter
  if (boost::json::parse(docs_spec) != boost::json::parse(response->second)) {
    std::ofstream("openapi-spec.served.json") << response->second;
    FAIL("docs/modules/dev/partials/spec.json is out of date: regenerate it from the served schema, saved as "
         "openapi-spec.served.json in the test dir (python3 -m json.tool --indent 2 openapi-spec.served.json)");
  }
}
