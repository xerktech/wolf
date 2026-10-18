#include <boost/asio/io_context.hpp>
#include <control/control.hpp>
#include <events/events.hpp>
#include <future>
#include <helpers/logger.hpp>
#include <immer/atom.hpp>
#include <runners/process.hpp>
#ifdef __linux__
#include <fcntl.h>
#include <linux/close_range.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace process {

using namespace wolf::core::events;

/**
 * Wolf's own descriptors (listening sockets, ENet, the API socket, ...) are not opened with close-on-exec, so without
 * this a spawned app would inherit them. A child that outlives Wolf would then keep its ports bound, and the next
 * Wolf fails with "bind: Address already in use".
 *
 * Runs in the forked child, right before exec: marks every fd above stdio as close-on-exec rather than closing it, so
 * Boost.Process' own (already close-on-exec) error-reporting pipe keeps working.
 */
struct close_inherited_fds : bp::extend::handler {
  template <class Executor> void on_exec_setup(Executor &) const {
#ifdef __linux__
    if (::close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) == 0) {
      return;
    }
    // Kernel < 5.11: fall back to marking each possible fd (fcntl and getrlimit are safe to call after fork)
    rlimit lim{};
    int max_fd = (::getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur != RLIM_INFINITY) ? lim.rlim_cur : 65536;
    for (int fd = 3; fd < max_fd; fd++) {
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
  }
};

void RunProcess::run(std::string_view session_id,
                     std::string_view app_state_folder,
                     std::string_view host_xdg_runtime_dir,
                     std::shared_ptr<events::devices_atom_queue> plugged_devices_queue,
                     const immer::array<std::string> &virtual_inputs,
                     const immer::array<std::pair<std::string, std::string>> &paths,
                     const immer::map<std::string, std::string> &env_variables,
                     std::string_view render_node) {
  logs::log(logs::debug, "[PROCESS] Starting process: {}", this->run_cmd);

  std::future<std::string> std_out = {}, err_out = {};
  boost::asio::io_context ios;
  bp::child child_proc;
  bp::group group_proc;

  try {
    auto env = boost::this_process::environment();
    for (const auto &env_var : env_variables) {
      env[env_var.first] = env_var.second;
    };

    child_proc = bp::child(this->run_cmd,
                           env,
                           bp::std_in.close(),
                           close_inherited_fds{},
#if BOOST_VERSION < 108800
                           bp::std_out > std_out,
                           bp::std_err > err_out,
                           ios,
#endif
                           group_proc);

  } catch (const std::system_error &e) {
    logs::log(logs::error, "Unable to start process, error: {} - {}", e.code().value(), e.what());
    return;
  }

  auto terminate_handler = this->ev_bus->register_handler<immer::box<StopStreamEvent>>(
      [&group_proc, session_id](const immer::box<StopStreamEvent> &terminate_ev) {
        if (std::to_string(terminate_ev->session_id) == session_id) {
          group_proc.terminate(); // Manually terminate the process
        }
      });

  auto terminate_lobby_handler = this->ev_bus->register_handler<immer::box<StopLobbyEvent>>(
      [&group_proc, session_id](const immer::box<StopLobbyEvent> &terminate_ev) {
        if (terminate_ev->lobby_id == session_id) {
          group_proc.terminate(); // Manually terminate the process
        }
      });

  // This will stop here until the process is over
#if BOOST_VERSION < 108800
  ios.run();
#endif
  child_proc.wait(); // to avoid a zombie process & get the exit code

  auto ex_code = child_proc.exit_code();
  if (ex_code != 0) {
    logs::log(logs::warning, "Process exited with code: {}", ex_code);
  } else {
    logs::log(logs::debug, "Process exited with code: {}", ex_code);
  }

  terminate_handler.unregister();
}

} // namespace process