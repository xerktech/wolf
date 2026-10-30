#include <api/api.hpp>
#include <helpers/utils.hpp>

namespace wolf::api {

using namespace wolf::core;

constexpr std::size_t MAX_WRITE_QUEUE_BYTES = 64 * 1024 * 1024;
// Headers + body of one request: the buffer used to be unbounded, so a huge Content-Length was read (or waited for)
// in full instead of being refused
constexpr std::size_t MAX_REQUEST_BYTES = 64 * 1024 * 1024;
// The request line and headers: each connection buffers them before anything else is known, so the cap is per
// connection and must stay small
constexpr std::size_t MAX_HEADER_BYTES = 64 * 1024;
// Bodies being read, summed over every connection: MAX_REQUEST_BYTES alone let N connections hold N x 64 MiB
constexpr std::size_t MAX_PENDING_BODY_BYTES = 2 * MAX_REQUEST_BYTES;

std::string to_str(boost::asio::streambuf &streambuf, std::size_t start, std::size_t end) {
  return {buffers_begin(streambuf.data()) + start, buffers_begin(streambuf.data()) + start + end};
}

std::string to_str(boost::asio::streambuf &streambuf, std::size_t end) {
  return to_str(streambuf, 0, end);
}

UnixSocketServer::UnixSocketServer(boost::asio::io_context &io_context,
                                   const std::string &socket_path,
                                   immer::box<state::AppState> app_state,
                                   std::chrono::milliseconds sse_keepalive_interval) {

  state_ = std::make_shared<UnixSocketState>(io_context, app_state, socket_path);
  state_->sse_keepalive_interval = sse_keepalive_interval;

  state_->http.add(HTTPMethod::GET,
                   "/api/v1/events",
                   {.summary = "Subscribe to events",
                    .description = "This endpoint allows clients to subscribe to events using SSE",
                    // TODO: json_schema = rfl::json::to_schema<EventsVariant>()
                    .handler = [this](auto req, auto socket) { endpoint_Events(req, socket); }});

  /**
   * Pairing API
   */

  state_->http.add(
      HTTPMethod::GET,
      "/api/v1/pair/pending",
      {
          .summary = "Get pending pair requests",
          .description = "This endpoint returns a list of Moonlight clients that are currently waiting to be paired.",
          .response_description = {{200, {.json_schema = rfl::json::to_schema<PendingPairRequestsResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_PendingPairRequest(req, socket); },
      });

  state_->http.add(HTTPMethod::POST,
                   "/api/v1/pair/client",
                   {
                       .summary = "Pair a client",
                       .request_description = APIDescription{.json_schema = rfl::json::to_schema<PairRequest>()},
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                                {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                                {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_Pair(req, socket); },
                   });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/unpair/client",
      {
          .summary = "Unpair a client",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<UnpairClientRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_UnpairClient(req, socket); },
      });

  state_->http.add(HTTPMethod::GET,
                   "/api/v1/clients",
                   {
                       .summary = "Get paired clients",
                       .description = "This endpoint returns a list of all paired clients.",
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<PairedClientsResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_PairedClients(req, socket); },
                   });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/clients/settings",
      {
          .summary = "Update client settings",
          .description = "Update a client's settings including app state folder and client-specific settings",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<UpdateClientSettingsRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_UpdateClientSettings(req, socket); },
      });

  /**
   * Apps API
   */

  state_->http.add(
      HTTPMethod::GET,
      "/api/v1/apps",
      {
          .summary = "Get all Moonlight apps",
          .description = "This endpoint returns a list of all apps that will be shown in the Moonlight client.",
          .response_description = {{200, {.json_schema = rfl::json::to_schema<AppListResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_Apps(req, socket); },
      });

  state_->http.add(HTTPMethod::POST,
                   "/api/v1/apps/add",
                   {
                       .summary = "Add a Moonlight app",
                       .request_description =
                           APIDescription{.json_schema = rfl::json::to_schema<rfl::Reflector<events::App>::ReflType>()},
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                                {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                                {409, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_AddApp(req, socket); },
                   });

  state_->http.add(HTTPMethod::POST,
                   "/api/v1/apps/delete",
                   {.summary = "Remove a Moonlight app",
                    .request_description = APIDescription{.json_schema = rfl::json::to_schema<AppDeleteRequest>()},
                    .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                             {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                             {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                    .handler = [this](auto req, auto socket) { endpoint_RemoveApp(req, socket); }});

  /**
   * Profiles API
   */

  state_->http.add(HTTPMethod::GET,
                   "/api/v1/profiles",
                   {
                       .summary = "Get all profiles",
                       .description = "This endpoint returns a list of all profiles.",
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<ProfileListResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_Profiles(req, socket); },
                   });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/profiles/add",
      {
          .summary = "Create a new profile",
          .request_description =
              APIDescription{.json_schema = rfl::json::to_schema<rfl::Reflector<events::Profile>::ReflType>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_AddProfile(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/profiles/remove",
      {
          .summary = "Remove a profile",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<ProfileRemoveRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_RemoveProfile(req, socket); },
      });

  /**
   * Stream session API
   */

  state_->http.add(
      HTTPMethod::GET,
      "/api/v1/sessions",
      {
          .summary = "Get all stream sessions",
          .description = "This endpoint returns a list of all active stream sessions.",
          .response_description = {{200, {.json_schema = rfl::json::to_schema<StreamSessionListResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessions(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/sessions/add",
      {
          .summary = "Create a new stream session",
          .request_description =
              APIDescription{.json_schema = rfl::json::to_schema<rfl::Reflector<events::StreamSession>::ReflType>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<StreamSessionCreated>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {409, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessionAdd(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/sessions/start",
      {
          .summary = "Start a stream session",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<StreamSessionStartRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessionStart(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/sessions/pause",
      {
          .summary = "Pause a stream session",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<StreamSessionPauseRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessionPause(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/sessions/stop",
      {
          .summary = "Stop a stream session",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<StreamSessionStopRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessionStop(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/sessions/input",
      {
          .summary = "Handle input for a stream session",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<StreamSessionHandleInputRequest>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_StreamSessionHandleInput(req, socket); },
      });

  state_->http.add(HTTPMethod::POST,
                   "/api/v1/runners/start",
                   {
                       .summary = "Start a runner in a given session",
                       .request_description = APIDescription{.json_schema = rfl::json::to_schema<RunnerStartRequest>()},
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                                {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                                {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_RunnerStart(req, socket); },
                   });

  /**
   * Lobbies API
   */

  state_->http.add(HTTPMethod::GET,
                   "/api/v1/lobbies",
                   {
                       .summary = "List all lobbies",
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<LobbiesResponse>()}},
                                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_Lobbies(req, socket); },
                   });

  state_->http.add(HTTPMethod::POST,
                   "/api/v1/lobbies/create",
                   {
                       .summary = "Create a new lobby",
                       .request_description = APIDescription{.json_schema = rfl::json::to_schema<CreateLobbyRequest>()},
                       .response_description = {{200, {.json_schema = rfl::json::to_schema<LobbyCreateResponse>()}},
                                                {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
                       .handler = [this](auto req, auto socket) { endpoint_LobbyCreate(req, socket); },
                   });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/lobbies/join",
      {
          .summary = "Join a lobby",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<events::JoinLobbyEvent>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {403, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {409, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_LobbyJoin(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/lobbies/leave",
      {
          .summary = "Leave a lobby",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<events::LeaveLobbyEvent>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_LobbyLeave(req, socket); },
      });

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/lobbies/stop",
      {
          .summary = "Stop a lobby",
          .request_description = APIDescription{.json_schema = rfl::json::to_schema<events::StopLobbyEvent>()},
          .response_description = {{200, {.json_schema = rfl::json::to_schema<GenericSuccessResponse>()}},
                                   {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {403, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {404, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                   {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
          .handler = [this](auto req, auto socket) { endpoint_LobbyStop(req, socket); },
      });

  /**
   * Utils
   */
  state_->http.add(HTTPMethod::GET,
                   "/api/v1/utils/get-icon",
                   {.summary = "Get the icon for a given app",
                    .description = "Get the icon for a given app, pass the icon_path as a query parameter ex: "
                                   "/api/v1/utils/get-icon?icon_path=/etc/wolf/icons/steam.png",
                    .handler = [this](auto req, auto socket) { endpoint_GetIcon(req, socket); }});

  state_->http.add(
      HTTPMethod::GET,
      "/api/v1/docker/images/inspect",
      {.summary = "Inspect a Docker image",
       .description = "Inspect a Docker image and returns the full JSON response as is from the Docker APIs at "
                      "/images/{image_name}/json expects image_name as a query parameter.",
       .handler = [this](auto req, auto socket) { endpoint_DockerInspectImage(req, socket); }});

  state_->http.add(
      HTTPMethod::POST,
      "/api/v1/docker/images/pull",
      {.summary = "Pull a Docker image",
       .description = "Pull a Docker image, will keep the connection open to send back progress updates. Each "
                      "progress event will be a single line encoded as JSON.",
       .request_description = APIDescription{.json_schema = rfl::json::to_schema<DockerPullImageRequest>()},
       .response_description = {{200, {.json_schema = rfl::json::to_schema<DockerPullImageResponse>()}},
                                {400, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}},
                                {500, {.json_schema = rfl::json::to_schema<GenericErrorResponse>()}}},
       .handler = [this](auto req, auto socket) { endpoint_DockerPullImage(req, socket); }});

  /**
   * OpenAPI schema
   */

  state_->http.add(HTTPMethod::GET,
                   "/api/v1/openapi-schema",
                   {.summary = "Return this OpenAPI schema as JSON", .handler = [this](auto req, auto socket) {
                      send_http(socket, 200, state_->http.openapi_schema());
                    }});

  state_->sse_keepalive_timer.async_wait([this](auto e) { sse_keepalive(e); });
  start_accept();
}

void UnixSocketServer::sse_keepalive(const boost::system::error_code &e) {
  if (e && e.value() != boost::asio::error::operation_aborted) {
    logs::log(logs::warning, "[API] Error in keepalive timer: {}", e.message());
    return;
  }
  cleanup_sockets();
  // Only idle connections need a keepalive. Queued behind a large backlog, it would also count towards
  // MAX_WRITE_QUEUE_BYTES and drop a subscriber that is still reading.
  // The timer runs on the io_context thread, which owns the sockets and their write queues.
  // Never destroyed: a static's destructor runs at exit while the io_context thread may still be copying it.
  static const auto &keepalive =
      *new std::shared_ptr<const std::string>(std::make_shared<const std::string>(":keepalive\n\n"));
  for (const auto &socket : state_->sockets) {
    if (socket->write_queue.empty()) {
      queue_write(socket, keepalive);
    }
  }
  state_->sse_keepalive_timer.expires_after(state_->sse_keepalive_interval);
  state_->sse_keepalive_timer.async_wait([this](auto e) { sse_keepalive(e); });
}

void UnixSocketServer::sse_broadcast(std::string payload) {
  // Events are fired from any thread: move onto the io_context thread before touching the list of sockets
  boost::asio::dispatch(state_->io_context,
                        [this, payload = std::make_shared<const std::string>(std::move(payload))]() {
                          for (const auto &socket : state_->sockets) {
                            queue_write(socket, payload);
                          }
                        });
}

void UnixSocketServer::broadcast_event(const std::string &event_type, const std::string &event_data) {
  sse_broadcast(fmt::format("event: {}\ndata: {}\n\n", event_type, event_data));
}

void UnixSocketServer::cleanup_sockets() {
  state_->sockets.erase(std::remove_if(state_->sockets.begin(),
                                       state_->sockets.end(),
                                       [](const auto &socket) { return !socket->is_alive; }),
                        state_->sockets.end());
}

void UnixSocketServer::send_http(std::shared_ptr<UnixSocket> socket, int status_code, std::string_view body) {
  if (body.empty()) {
    send_http(socket, status_code, {{"Content-Length: 0"}}, body);
  } else { // Every non-empty reply sent through here is JSON
    send_http(socket,
              status_code,
              {{"Content-Length: " + std::to_string(body.size())}, {"Content-Type: application/json"}},
              body);
  }
}

static std::string_view status_reason(int status_code) {
  switch (status_code) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 409:
    return "Conflict";
  case 413:
    return "Payload Too Large";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 503:
    return "Service Unavailable";
  default:
    return "Unknown";
  }
}

void UnixSocketServer::send_http(std::shared_ptr<UnixSocket> socket,
                                 int status_code,
                                 const std::vector<std::string_view> &http_headers,
                                 std::string_view body) {
  auto http_reply = fmt::format("HTTP/1.0 {} {}\r\n{}\r\n\r\n{}",
                                status_code,
                                status_reason(status_code),
                                fmt::join(http_headers, "\r\n"),
                                body);
  send_data(socket, std::move(http_reply));
}

void UnixSocketServer::send_data(std::shared_ptr<UnixSocket> socket, std::string data) {
  // Some endpoints reply from their own thread: move onto the io_context thread, which owns the write queue
  boost::asio::dispatch(
      state_->io_context,
      [this, socket, data = std::make_shared<const std::string>(std::move(data))]() { queue_write(socket, data); });
}

void UnixSocketServer::queue_write(const std::shared_ptr<UnixSocket> &socket, std::shared_ptr<const std::string> data) {
  // The write queue belongs to the io_context thread. Callers should already be on it, but a racy queue only shows
  // under TSan or a stress rig, so a caller on another thread is moved there instead of corrupting the queue.
  if (!state_->io_context.get_executor().running_in_this_thread()) {
    boost::asio::post(state_->io_context, [this, socket, data = std::move(data)]() { queue_write(socket, data); });
    return;
  }
  if (!socket->is_alive) {
    return;
  }
  // A client that stops reading (e.g. an SSE subscriber) would otherwise make us buffer every event forever
  // Only the backlog behind the write in flight counts, and only once it's already over the limit: a client that
  // is less than MAX_WRITE_QUEUE_BYTES behind gets the next message, however big it is.
  // A single queued message bigger than the limit counts as backlog too, so the client will be dropped if anything
  // else queues behind it before it's sent; API messages are far below that size.
  if (!socket->write_queue.empty() &&
      socket->write_queue_bytes - socket->write_queue.front()->size() > MAX_WRITE_QUEUE_BYTES) {
    logs::log(logs::warning,
              "[API] Client is not reading, dropping it after {} queued bytes",
              socket->write_queue_bytes);
    // The queue is released by the completion of the write in flight, which might already be pending.
    // Not cleanup_sockets(): we might be inside the sse_broadcast loop over the sockets
    close(*socket);
    return;
  }
  socket->write_queue_bytes += data->size();
  socket->write_queue.push_back(std::move(data));
  if (socket->write_queue.size() == 1) { // Otherwise the write in flight will pick it up once done
    write_next(socket);
  }
}

void UnixSocketServer::write_next(const std::shared_ptr<UnixSocket> &socket) {
  boost::asio::async_write(socket->socket,
                           boost::asio::buffer(*socket->write_queue.front()),
                           [this, socket](const boost::system::error_code &ec, std::size_t /*length*/) {
                             if (ec || !socket->is_alive) {
                               if (ec) {
                                 logs::log(logs::debug, "[API] Error sending data: {}", ec.message());
                               }
                               socket->write_queue.clear();
                               socket->write_queue_bytes = 0;
                               close(*socket);
                               cleanup_sockets();
                               return;
                             }
                             socket->write_queue_bytes -= socket->write_queue.front()->size();
                             socket->write_queue.pop_front();
                             if (!socket->write_queue.empty()) {
                               write_next(socket);
                             }
                           });
}

void UnixSocketServer::handle_request(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  // Bodies can be megabytes (e.g. apps with inline pipelines): only log the start
  logs::log(logs::debug,
            "[API] Received request: {} {} - {}",
            rfl::enum_to_string(req.method),
            req.path,
            std::string_view(req.body).substr(0, 1024));

  if (!state_->http.handle_request(req, socket)) {
    // Not closed here: that would cancel the reply's write, still in flight. Like every other reply, the socket
    // closes once the write completes and the last reference to it goes.
    send_http(socket, 404, "");
  }
}

void UnixSocketServer::start_connection(std::shared_ptr<UnixSocket> socket) {
  auto request_buf = std::make_shared<boost::asio::streambuf>(MAX_HEADER_BYTES);
  boost::asio::async_read_until(
      socket->socket,
      *request_buf,
      "\r\n\r\n",
      [this, socket, request_buf](const boost::system::error_code &ec, std::size_t bytes_transferred) {
        if (ec == boost::asio::error::not_found) { // the buffer filled up before the end of the headers
          send_http(socket, 431, rfl::json::write(GenericErrorResponse{.error = "Request Header Fields Too Large"}));
          return;
        } else if (ec) {
          logs::log(logs::error, "[API] Error reading request: {}", ec.message());
          close(*socket);
          return;
        }
        HTTPRequest req = {};
        std::string method;
        auto headers = to_str(*request_buf, bytes_transferred);
        std::istringstream is(headers);
        SimpleWeb::RequestMessage::parse(is, method, req.path, req.query_string, req.http_version, req.headers);
        if (method == "GET")
          req.method = HTTPMethod::GET;
        else if (method == "POST")
          req.method = HTTPMethod::POST;
        else if (method == "PUT")
          req.method = HTTPMethod::PUT;
        else if (method == "DELETE")
          req.method = HTTPMethod::DELETE;
        else
          req.method = HTTPMethod::GET;

        if (req.headers.contains("Transfer-Encoding") && req.headers.find("Transfer-Encoding")->second == "chunked") {
          logs::log(logs::error, "[API] Chunked encoding not supported, use HTTP/1.0 instead");
          send_http(
              socket,
              501,
              rfl::json::write(GenericErrorResponse{.error = "Chunked encoding not supported, use HTTP/1.0 instead"}));
          return;
        }

        // Get the body payload
        if (req.headers.contains("Content-Length")) {
          auto parsed_content_length = utils::to_size_t(req.headers.find("Content-Length")->second);
          if (!parsed_content_length) {
            send_http(socket, 400, rfl::json::write(GenericErrorResponse{.error = "Invalid Content-Length"}));
            return;
          }
          auto content_length = *parsed_content_length;
          if (content_length > MAX_REQUEST_BYTES - bytes_transferred) {
            send_http(socket, 413, rfl::json::write(GenericErrorResponse{.error = "Payload Too Large"}));
            return;
          }
          // Read straight into the body's string, which grows as bytes arrive: reserving doesn't touch the pages, so
          // a Content-Length alone costs no memory, and nothing is copied out of a streambuf afterwards
          auto body = std::make_shared<std::string>();
          auto already_read = std::min(request_buf->size() - bytes_transferred, content_length);
          body->append(buffers_begin(request_buf->data()) + bytes_transferred,
                       buffers_begin(request_buf->data()) + bytes_transferred + already_read);
          // read_until stops at the end of the headers, often a few hundred bytes in: take what already arrived too
          boost::system::error_code ec_available;
          if (auto available = socket->socket.available(ec_available); !ec_available && available > 0) {
            auto ready = std::min(available, content_length - already_read);
            body->reserve(already_read + ready);
            // Doesn't block: the bytes are already in the socket's buffer
            already_read += boost::asio::read(socket->socket,
                                              boost::asio::dynamic_buffer(*body, content_length),
                                              boost::asio::transfer_exactly(ready),
                                              ec_available);
          }
          if (already_read == content_length) {
            req.body = std::move(*body);
            handle_request(req, socket);
            return;
          }
          // Only a body that still has to be read counts (whole: what was already read stays held meanwhile), so
          // idle connections holding the budget never block a request that arrived whole. Only this (io_context)
          // thread touches the counter.
          auto to_read = content_length - already_read;
          if (content_length > MAX_PENDING_BODY_BYTES - state_->pending_body_bytes) {
            logs::log(logs::warning, "[API] Too many request bodies being read at once, refusing one");
            send_http(socket, 503, rfl::json::write(GenericErrorResponse{.error = "Too many large requests at once"}));
            return;
          }
          body->reserve(content_length);
          state_->pending_body_bytes += content_length;
          boost::asio::async_read(
              socket->socket,
              boost::asio::dynamic_buffer(*body, content_length),
              boost::asio::transfer_exactly(to_read),
              [this, content_length, socket, body, req = std::make_unique<HTTPRequest>(std::move(req))](
                  const boost::system::error_code &ec,
                  std::size_t /*bytes_transferred*/) {
                state_->pending_body_bytes -= content_length; // errors included
                if (ec) {
                  logs::log(logs::error, "[API] Error reading request body: {}", ec.message());
                  close(*socket);
                  return;
                }
                req->body = std::move(*body);
                handle_request(*req, socket);
              });
        } else {
          handle_request(req, socket);
        }
      });
}

void UnixSocketServer::start_accept() {
  auto socket = std::make_shared<UnixSocket>(
      UnixSocket{.socket = boost::asio::local::stream_protocol::socket(state_->io_context)});
  state_->acceptor.async_accept(socket->socket, [this, socket](const boost::system::error_code &ec) {
    if (!ec) {
      start_accept();           // Immediately start accepting a new connection
      start_connection(socket); // Start reading the request from the new connection
    } else {
      logs::log(logs::error, "[API] Error accepting connection: {}", ec.message());
      close(*socket);
    }
  });
}

void UnixSocketServer::close(UnixSocket &socket) {
  socket.socket.close();
  socket.is_alive = false;
}
} // namespace wolf::api