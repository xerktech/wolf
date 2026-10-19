#include <core/audio.hpp>
#include <helpers/logger.hpp>
#include <memory>
#include <pulse/pulseaudio.h>
#include <vector>

namespace fmt {
template <> class formatter<wolf::core::audio::AudioMode::Speakers> {
public:
  template <typename ParseContext> constexpr auto parse(ParseContext &ctx) {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(const wolf::core::audio::AudioMode::Speakers &speaker, FormatContext &ctx) const {
    std::string speaker_name;
    // Mapping taken from
    // https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/User/Modules/#module-null-sink
    switch (speaker) {
    case wolf::core::audio::AudioMode::Speakers::FRONT_LEFT:
      speaker_name = "front-left";
      break;
    case wolf::core::audio::AudioMode::Speakers::FRONT_RIGHT:
      speaker_name = "front-right";
      break;
    case wolf::core::audio::AudioMode::Speakers::FRONT_CENTER:
      speaker_name = "front-center";
      break;
    case wolf::core::audio::AudioMode::Speakers::LOW_FREQUENCY:
      speaker_name = "lfe";
      break;
    case wolf::core::audio::AudioMode::Speakers::BACK_LEFT:
      speaker_name = "rear-left";
      break;
    case wolf::core::audio::AudioMode::Speakers::BACK_RIGHT:
      speaker_name = "rear-right";
      break;
    case wolf::core::audio::AudioMode::Speakers::SIDE_LEFT:
      speaker_name = "side-left";
      break;
    case wolf::core::audio::AudioMode::Speakers::SIDE_RIGHT:
      speaker_name = "side-right";
      break;
    case wolf::core::audio::AudioMode::Speakers::MAX_SPEAKERS:
      break;
    }
    return fmt::format_to(ctx.out(), "{}", speaker_name);
  }
};
} // namespace fmt

namespace wolf::core::audio {

struct Server {
  pa_context *ctx;
  // A threaded mainloop: libpulse objects are not thread-safe, so every call into ctx from outside the mainloop
  // thread must hold this loop's lock (see MainloopLock). Callbacks already run on the mainloop thread with it held.
  pa_threaded_mainloop *loop;
  boost::promise<bool> on_ready;
  boost::future<bool> on_ready_fut;
  // Guards on_ready against being set more than once. The PA state callback
  // fires for every state change, and READY/TERMINATED/FAILED all resolve the
  // promise; without this, a connection that goes READY and later TERMINATED
  // (e.g. the PulseAudio server going away) would call set_value twice and
  // throw promise_already_satisfied. Only the single PA mainloop thread sets
  // it, so a plain bool is sufficient. Reset in disconnect() when the promise
  // is recreated.
  bool ready_set = false;
  // Operations queued while still connecting; run by the state callback once READY. Guarded by the mainloop lock.
  std::vector<std::function<void()>> pending_ops;
};

/**
 * Holds the PA mainloop lock for its lifetime. The lock is recursive, but must not be taken from the mainloop thread
 * itself (callbacks there already hold it), so this is a no-op when constructed on that thread.
 */
class MainloopLock {
public:
  explicit MainloopLock(pa_threaded_mainloop *loop) : loop(loop), locked(!pa_threaded_mainloop_in_thread(loop)) {
    if (locked) {
      pa_threaded_mainloop_lock(loop);
    }
  }
  ~MainloopLock() {
    if (locked) {
      pa_threaded_mainloop_unlock(loop);
    }
  }
  MainloopLock(const MainloopLock &) = delete;
  MainloopLock &operator=(const MainloopLock &) = delete;

private:
  pa_threaded_mainloop *loop;
  bool locked;
};

pa_context *context(const std::shared_ptr<Server> &server) {
  return server ? server->ctx : nullptr;
}

std::shared_ptr<Server> connect(std::string_view server) {
  {
    auto loop = pa_threaded_mainloop_new();
    auto ctx = pa_context_new(pa_threaded_mainloop_get_api(loop), "wolf");
    auto state = std::make_shared<Server>(Server{.ctx = ctx, .loop = loop, .on_ready = boost::promise<bool>()});
    state->on_ready_fut = state->on_ready.get_future();

    pa_context_set_state_callback(
        ctx,
        [](pa_context *ctx, void *data) {
          auto state = (Server *)data;

          switch (pa_context_get_state(ctx)) {
          case PA_CONTEXT_READY: {
            logs::log(logs::debug, "[PULSE] Pulse connection ready");
            if (!state->ready_set) {
              state->ready_set = true;
              state->on_ready.set_value(true);
            }
            auto ops = std::move(state->pending_ops);
            state->pending_ops.clear();
            for (const auto &op : ops) {
              op();
            }
            break;
          }
          case PA_CONTEXT_TERMINATED:
            logs::log(logs::debug, "[PULSE] Terminated connection");
            state->pending_ops.clear();
            if (!state->ready_set) {
              state->ready_set = true;
              state->on_ready.set_value(false);
            }
            break;
          case PA_CONTEXT_FAILED:
            logs::log(logs::debug, "[PULSE] Context failed");
            state->pending_ops.clear();
            if (!state->ready_set) {
              state->ready_set = true;
              state->on_ready.set_value(false);
            }
            break;
          case PA_CONTEXT_CONNECTING:
            logs::log(logs::debug, "[PULSE] Connecting...");
          case PA_CONTEXT_UNCONNECTED:
          case PA_CONTEXT_AUTHORIZING:
          case PA_CONTEXT_SETTING_NAME:
            break;
          }
        },
        state.get());

    // The mainloop thread isn't running yet, so no lock is needed until pa_threaded_mainloop_start()
    auto err = pa_context_connect(ctx, server.empty() ? nullptr : server.data(), PA_CONTEXT_NOFLAGS, nullptr);
    if (err) {
      logs::log(logs::warning, "[PULSE] Unable to connect, {}", pa_strerror(err));
    }

    if (pa_threaded_mainloop_start(loop) < 0) {
      logs::log(logs::warning, "[PULSE] Can't run PA mainloop");
    }

    return state;
  }
}

bool connected(const std::shared_ptr<Server> &server) {
  return server->on_ready_fut.get();
}

void queue_op(const std::shared_ptr<Server> &server, const std::function<void()> &op) {
  MainloopLock lock(server->loop);
  auto state = pa_context_get_state(server->ctx);
  if (state == PA_CONTEXT_READY) {
    op();
  } else if (PA_CONTEXT_IS_GOOD(state)) { // Still connecting, run it from the state callback once READY
    server->pending_ops.push_back(op);
  } // else: the connection failed or was closed, the op is dropped
}

/**
 * `ref` is owned by `op`'s result callback, which deletes it. When the connection fails or closes while the request is
 * pending, libpulse cancels the operation without ever calling that callback: release `ref` then instead, or it leaks
 * (and anything waiting on what it holds, like a promise, is never woken up).
 * Must be called with the mainloop lock held, right after the request was sent.
 */
template <typename T> static void release_if_cancelled(pa_operation *op, std::shared_ptr<T> *ref) {
  pa_operation_set_state_callback(
      op,
      [](pa_operation *o, void *data) {
        if (pa_operation_get_state(o) == PA_OPERATION_CANCELLED) {
          delete static_cast<std::shared_ptr<T> *>(data);
        }
      },
      ref);
}

/**
 * Must be called with the mainloop lock held; doesn't wait for the module to be unloaded.
 */
static void unload_sink(pa_context *ctx, uint32_t sink_idx) {
  if (sink_idx == PA_INVALID_INDEX) {
    logs::log(logs::debug, "[PULSE] Virtual sink was never created, nothing to remove");
    return;
  }
  auto operation = pa_context_unload_module(
      ctx,
      sink_idx,
      [](pa_context *c, int status, void *data) {
        logs::log(logs::debug, "[PULSE] Removed virtual sink, status: {}", status);
      },
      nullptr);
  if (operation) {
    pa_operation_unref(operation);
  }
}

std::shared_ptr<VSink> create_virtual_sink(const std::shared_ptr<Server> &server, const AudioDevice &device) {

  auto vsink = std::make_shared<VSink>();
  vsink->device = device;

  // Formatted now: device.sink_name is a view of the caller's string, which is gone by the time a queued op runs
  auto channel_spec = fmt::format("rate={} sink_name={} channels={} channel_map={}",
                                  device.mode.sample_rate,
                                  device.sink_name,
                                  device.mode.channels,
                                  fmt::join(device.mode.speakers, ","));
  queue_op(server, [server, vsink, channel_spec]() {
    auto vsink_ref = new std::shared_ptr<VSink>(vsink); // released by the callback below
    auto operation = pa_context_load_module(
        server->ctx,
        "module-null-sink",
        channel_spec.c_str(),
        [](pa_context *c, uint32_t idx, void *data) {
          // Owns a reference: delete_virtual_sink() no longer waits for this callback, so the session may already
          // have dropped its last reference to the VSink by the time PulseAudio replies.
          std::unique_ptr<std::shared_ptr<VSink>> result((std::shared_ptr<VSink> *)data);
          logs::log(logs::debug, "[PULSE] Created virtual sink: {}", idx);
          (*result)->sink_idx.set_value(idx);
          if ((*result)->unload_on_load) { // delete_virtual_sink() ran before the module was loaded
            unload_sink(c, idx);
          }
        },
        vsink_ref);
    if (operation) {
      release_if_cancelled(operation, vsink_ref);
      pa_operation_unref(operation);
    } else { // The callback will never run
      logs::log(logs::warning, "[PULSE] Unable to create virtual sink: {}", pa_strerror(pa_context_errno(server->ctx)));
      delete vsink_ref;
    }
  });

  return vsink;
}

void delete_virtual_sink(const std::shared_ptr<Server> &server, const std::shared_ptr<VSink> &vsink) {
  if (vsink->deleted.exchange(true)) {
    logs::log(logs::debug, "[PULSE] Virtual sink already removed, skipping");
    return;
  }
  // Never block here waiting for the index: this runs with the mainloop lock held, and the load callback that sets
  // the index can only run on the mainloop thread once the lock is released.
  queue_op(server, [server, vsink]() {
    if (vsink->sink_idx_fut.is_ready()) {
      unload_sink(server->ctx, vsink->sink_idx_fut.get());
    } else {
      vsink->unload_on_load = true;
    }
  });
}

/**
 * The sink_name=<name> argument of a module-null-sink as create_virtual_sink() loads it, or "" if it has none.
 */
static std::string_view null_sink_name(std::string_view module_args) {
  constexpr std::string_view key = "sink_name=";
  for (auto pos = module_args.find(key); pos != std::string_view::npos; pos = module_args.find(key, pos + 1)) {
    if (pos == 0 || module_args[pos - 1] == ' ') {
      auto value = module_args.substr(pos + key.size());
      return value.substr(0, value.find(' '));
    }
  }
  return {};
}

std::vector<unsigned int> find_virtual_sinks(const std::shared_ptr<Server> &server,
                                             std::string_view sink_name_prefix,
                                             std::chrono::milliseconds timeout) {
  struct Request {
    std::string prefix;
    std::vector<unsigned int> modules;
    boost::promise<std::vector<unsigned int>> done; // broken (destroyed unset) if the request is dropped or cancelled
  };
  auto request = std::make_shared<Request>(Request{.prefix = std::string(sink_name_prefix)});
  auto result = request->done.get_future();
  queue_op(server, [server, request = std::move(request)]() {
    auto request_ref = new std::shared_ptr<Request>(request); // released at eol
    auto operation = pa_context_get_module_info_list(
        server->ctx,
        [](pa_context *c, const pa_module_info *info, int eol, void *data) {
          auto request = static_cast<std::shared_ptr<Request> *>(data);
          if (eol) { // the last call, also on error
            (*request)->done.set_value((*request)->modules);
            delete request;
            return;
          }
          if (info && info->name && info->argument && std::string_view(info->name) == "module-null-sink" &&
              null_sink_name(info->argument).starts_with((*request)->prefix)) {
            logs::log(logs::info,
                      "[PULSE] Found leftover virtual sink: {} (module {})",
                      null_sink_name(info->argument),
                      info->index);
            (*request)->modules.push_back(info->index);
          }
        },
        request_ref);
    if (operation) {
      release_if_cancelled(operation, request_ref);
      pa_operation_unref(operation);
    } else { // The callback will never run
      delete request_ref;
    }
  });
  if (result.wait_for(boost::chrono::milliseconds(timeout.count())) == boost::future_status::ready) {
    try {
      return result.get();
    } catch (const std::exception &) { // broken promise: the connection went away first
    }
  }
  return {};
}

void unload_virtual_sinks(const std::shared_ptr<Server> &server, const std::vector<unsigned int> &modules) {
  queue_op(server, [server, modules]() {
    for (auto module : modules) {
      logs::log(logs::info, "[PULSE] Removing leftover virtual sink (module {})", module);
      unload_sink(server->ctx, module);
    }
  });
}

bool flush(const std::shared_ptr<Server> &server, std::chrono::milliseconds timeout) {
  auto done = std::make_shared<boost::promise<void>>();
  auto done_fut = done->get_future();
  // Only the op may own the promise: when queue_op drops it (connection gone) the promise is destroyed unset, which
  // breaks it and makes the future ready right away instead of waiting out the timeout.
  queue_op(server, [server, done = std::move(done)]() {
    // PulseAudio answers a connection's requests in order: once this one is answered, every earlier one was handled
    auto done_ref = new std::shared_ptr<boost::promise<void>>(done);
    auto operation = pa_context_get_server_info(
        server->ctx,
        [](pa_context *c, const pa_server_info *info, void *data) {
          std::unique_ptr<std::shared_ptr<boost::promise<void>>> done((std::shared_ptr<boost::promise<void>> *)data);
          (*done)->set_value();
        },
        done_ref);
    if (operation) {
      release_if_cancelled(operation, done_ref); // breaks the promise, so a dying server doesn't wait out the timeout
      pa_operation_unref(operation);
    } else { // The callback will never run
      delete done_ref;
    }
  });
  return done_fut.wait_for(boost::chrono::milliseconds(timeout.count())) == boost::future_status::ready;
}

void disconnect(const std::shared_ptr<Server> &server) {
  MainloopLock lock(server->loop);
  server->on_ready = boost::promise<bool>(); // Creates a new promise
  server->ready_set = false;                 // ...so allow it to be set again
  pa_context_disconnect(server->ctx);
}

std::string get_server_name(const std::shared_ptr<Server> &server) {
  MainloopLock lock(server->loop);
  auto name = pa_context_get_server(server->ctx);
  return name ? name : "";
}

} // namespace wolf::core::audio
