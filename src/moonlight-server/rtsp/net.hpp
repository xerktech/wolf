#pragma once

#include "boost/algorithm/hex.hpp"
#include <boost/asio.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/shared_ptr.hpp>
#include <rtsp/commands.hpp>
#include <rtsp/encryption.hpp>
#include <state/sessions.hpp>
#include <string_view>
#include <thread>
#include <utility>

namespace rtsp {

namespace asio = boost::asio;
using asio::ip::tcp;
using namespace std::string_view_literals;

/**
 * A wrapper on top of the basic boost socket, it'll be in charge of sending and receiving RTSP messages
 * based on the tutorial at: https://www.boost.org/doc/libs/1_79_0/doc/html/boost_asio/tutorial/tutdaytime3.html
 *
 * Bear in mind that the basic methods of this class can be trivially used to also implement the RTSP client, see the
 * testRTSP class for an example client implementation.
 */
class tcp_connection : public boost::enable_shared_from_this<tcp_connection> {
public:
  typedef boost::shared_ptr<tcp_connection> pointer;

  static pointer create(boost::asio::io_context &io_context, const state::SessionsAtoms &stream_sessions) {
    return pointer(new tcp_connection(io_context, stream_sessions));
  }

  auto &socket() {
    return socket_;
  }

  /**
   * Cleanly closing the socket, see: https://stackoverflow.com/questions/1993216/boostasio-cleanly-disconnecting
   *
   * Moonlight detects you're done writing the message by waiting for the TCP FIN that is generated when you gracefully
   * close your socket. It looks like Boost's default behavior for close() is that it performs an abortive close
   * which drops any outstanding data and sends a RST to the remote host if there's any unacked data remaining
   * in the send buffer.
   * So effectively, it's a race condition whether the client gets all the data and acks first or the socket is
   * closed and a RST packet is sent (which causes the ECONNRESET error).
   *
   * - @cgutman
   */
  void close() {
    logs::log(logs::trace, "[RTSP] closing socket");
    // Use the non-throwing overloads. If the peer has already gone away (it
    // sent a malformed request and reset, or simply hung up) the socket is no
    // longer connected and shutdown()/close() would throw — e.g. ENOTCONN,
    // "Transport endpoint is not connected". That exception unwinds out of
    // io_context.run() in run_server() and tears down the whole RTSP server,
    // so a single early-disconnecting client stops all future streaming.
    // Swallowing the error keeps the server alive.
    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
  }

  /**
   * True if both strings are the same address, so that an API caller passing an IPv4-mapped IPv6 client_ip
   * (::ffff:a.b.c.d) still matches the plain IPv4 peer address that our IPv4-only acceptor reports.
   */
  static bool same_ip(std::string_view a, std::string_view b) {
    auto normalise = [](std::string_view ip) {
      boost::system::error_code ec;
      auto address = asio::ip::make_address(ip, ec);
      if (ec) {
        return std::string(ip);
      }
      if (address.is_v6() && address.to_v6().is_v4_mapped()) {
        return asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6()).to_string();
      }
      return address.to_string();
    };
    return normalise(a) == normalise(b);
  }

  static std::optional<events::StreamSession> get_session(const immer::vector<events::StreamSession> &sessions,
                                                          const RTSP_PACKET &packet,
                                                          std::string_view user_ip) {
    std::string host_option = "";
    if (auto host = packet.options.find("Host"); host != packet.options.end()) {
      host_option = host->second;
    }
    std::optional<events::StreamSession> rejected;
    for (const events::StreamSession &session : sessions) {
      if (session.rtsp_encrypted) { // Only reachable via encrypted RTSP, see get_encrypted_session()
        continue;
      }
      bool fake_ip_match = session.rtsp_fake_ip == packet.request.uri.ip || host_option == session.rtsp_fake_ip;
      // RTSP is unauthenticated and the fake IP is no secret (e.g. an orchestrator may set it to the pod IP), so
      // it only picks between the sessions of the client that is talking to us: a request from any other address
      // could otherwise read the session's ping payload (and steal its video) or ANNOUNCE before the real client.
      if (!same_ip(session.ip, user_ip)) {
        if (fake_ip_match) {
          rejected = session;
        }
        continue;
      }
      if (fake_ip_match) {
        logs::log(logs::debug, "[RTSP] found session by matching payload: {}", session.rtsp_fake_ip);
        return session;
      } else if (host_option == "0.0.0.0" || host_option.empty()) {
        logs::log(logs::debug, "[RTSP] found session by matching IP: {}", session.ip);
        return session;
      }
    }
    if (rejected) {
      logs::log(logs::warning,
                "[RTSP] rejecting request for {} from {}, the session belongs to {}",
                rejected->rtsp_fake_ip,
                user_ip,
                rejected->ip);
    }
    return std::nullopt;
  }

  /**
   * An encrypted message carries no plaintext session hint, so it belongs to the (encrypted RTSP) session whose key
   * authenticates it. Only the paired client that launched that session knows its key.
   *
   * @return the matching session and the decrypted RTSP message
   */
  static std::optional<std::pair<events::StreamSession, std::string>>
  get_encrypted_session(const immer::vector<events::StreamSession> &sessions, std::string_view raw_msg) {
    for (const events::StreamSession &session : sessions) {
      if (!session.rtsp_encrypted) {
        continue;
      }
      if (auto plaintext =
              encryption::unseal(raw_msg, crypto::hex_to_str(session.aes_key, true), encryption::CLIENT_ORIGINATED)) {
        logs::log(logs::debug, "[RTSP] found session by encryption key: {}", session.session_id);
        return std::make_pair(session, std::move(*plaintext));
      }
    }
    return std::nullopt;
  }

  /**
   * Will start the following (async) chain:
   *  1- wait for a message
   *  2- once received, parse it
   *  3- call the corresponding rtsp::command
   *  4- send back the response message
   */
  void start() {
    // Resolve the peer address up front and non-throwing. A client that sent a
    // request and immediately reset (Moonlight does this on a rejected message)
    // leaves the socket disconnected, and remote_endpoint() would otherwise
    // throw ENOTCONN — which, thrown from this async handler, would unwind out
    // of io_context.run() and take the whole RTSP server down. If the peer is
    // already gone there is nothing to serve, so bail cleanly.
    boost::system::error_code ep_ec;
    auto remote = socket_.remote_endpoint(ep_ec);
    if (ep_ec) {
      logs::log(logs::debug, "[RTSP] connection gone before processing: {}", ep_ec.message());
      close();
      return;
    }
    std::string user_ip = remote.address().to_string();
    logs::log(logs::trace, "[RTSP] received connection from IP: {}", user_ip);
    receive_raw([self = shared_from_this(), user_ip](std::optional<std::string> raw_msg) {
      if (raw_msg && encryption::is_encrypted(*raw_msg)) {
        return self->handle_encrypted(*raw_msg, user_ip);
      }
      auto parsed_msg = raw_msg ? rtsp::parse(*raw_msg) : std::nullopt;
      if (parsed_msg) {
        auto session = get_session(self->stream_sessions->load(), parsed_msg.value(), user_ip);
        if (session) {
          auto response = commands::message_handler(parsed_msg.value(), session.value());
          self->send_message(response, [self](auto bytes) { self->close(); });
        } else {
          logs::log(logs::warning, "[RTSP] received packet from unrecognised client: {}", user_ip);
          self->close();
        }
      } else {
        logs::log(logs::error, "[RTSP] error parsing message");
        self->send_message((rtsp::commands::error_msg(400, "BAD REQUEST")), [self](auto bytes) { self->close(); });
      }
    });
  }

  void handle_encrypted(std::string_view raw_msg, const std::string &user_ip) {
    auto found = get_encrypted_session(stream_sessions->load(), raw_msg);
    if (!found) {
      // We can't even answer: there's no key to seal a reply with
      logs::log(logs::warning, "[RTSP] received encrypted packet that no session's key opens, from: {}", user_ip);
      close();
      return;
    }
    auto &[session, plaintext] = *found;
    auto parsed_msg = rtsp::parse(plaintext);
    auto response = parsed_msg ? commands::message_handler(parsed_msg.value(), session)
                               : rtsp::commands::error_msg(400, "BAD REQUEST");
    if (!parsed_msg) {
      logs::log(logs::error, "[RTSP] error parsing encrypted message");
    }
    auto raw_response = rtsp::to_string(response);
    logs::log(logs::trace, "[RTSP] sending encrypted reply: \n{}", raw_response);
    auto sealed = encryption::seal(raw_response,
                                   crypto::hex_to_str(session.aes_key, true),
                                   ++encryption_seq_,
                                   encryption::HOST_ORIGINATED);
    send_raw(std::move(sealed), [self = shared_from_this()](auto bytes) { self->close(); });
  }

  void receive_message(const std::function<void(std::optional<RTSP_PACKET>)> &on_msg_read) {
    receive_raw([on_msg_read](std::optional<std::string> raw_msg) {
      on_msg_read(raw_msg ? rtsp::parse(*raw_msg) : std::nullopt);
    });
  }

  /**
   * We have no way to know the size of the message and there's no special sequence of characters to delimit the very
   * end (including payload). So we'll try to read everything that has been sent, we enforce a max message size AND a
   * timeout in order to avoid stalling.
   *
   * Timeout is adapted from:
   * https://www.boost.org/doc/libs/1_79_0/doc/html/boost_asio/example/cpp11/timeouts/async_tcp_client.cpp
   *
   * @return Will call the callback passing the full raw message (plaintext or still encrypted), or nullopt when it's
   * too big or doesn't fully arrive before the deadline
   *
   * @note: an encrypted message (see rtsp/encryption.hpp) states its length in its header instead.
   *
   * @note: ANNOUNCE messages will be bigger than the default 512 bytes that boost reads, fortunately we receive an
   * option in the message: Content-length which represent the size in bytes of the payload. We'll keep recursively call
   * receive_raw() until our payload size matches the specified Content-length
   */
  void receive_raw(const std::function<void(std::optional<std::string>)> &on_msg_read) {
    // One deadline for the whole message, armed on its first read only: re-arming it on every read would let a
    // client trickling a byte at a time hold the connection open indefinitely.
    // Set the expiry BEFORE waiting: expires_after() cancels any pending wait.
    if (prev_read_bytes_ == 0) {
      deadline_.expires_after(std::chrono::milliseconds(timeout_millis));
      deadline_.async_wait([self = shared_from_this()](auto error) {
        if (!error && self->deadline_.expiry() <= asio::steady_timer::clock_type::now()) { // The deadline has passed
          logs::log(logs::trace, "[RTSP] deadline over");
          self->socket_.cancel();
        }
      });
    }

    boost::asio::async_read(
        socket(),
        streambuf_,
        boost::asio::transfer_at_least(1),
        [self = shared_from_this(), on_msg_read](auto error_code, auto bytes_transferred) {
          if (error_code &&
              error_code != boost::asio::error::operation_aborted) { // it'll be aborted when the deadline expires
            logs::log(logs::error, "[RTSP] error during transmission: {}", error_code.message());
            self->deadline_.cancel();
            self->send_message(rtsp::commands::error_msg(400, "BAD REQUEST"), [self](auto bytes) { self->close(); });
            return;
          }
          // Hands the (possibly invalid) message over, once it's complete or we've given up on it
          auto finish = [&self, &on_msg_read](std::optional<std::string> msg) {
            self->deadline_.cancel();
            self->prev_read_ = "";
            self->prev_read_bytes_ = 0;
            on_msg_read(msg);
          };
          // Check the expiry here too, not just the timer's cancel(): if this read had already completed when the
          // timer fired, the cancel() hit nothing and the message would never time out.
          bool timed_out = error_code == boost::asio::error::operation_aborted ||
                           self->deadline_.expiry() <= asio::steady_timer::clock_type::now();
          std::string raw_msg = {std::istreambuf_iterator<char>(&self->streambuf_), {}};
          logs::log(logs::trace, "[RTSP] received message {} bytes \n{}", bytes_transferred, raw_msg);

          auto full_raw_msg = self->prev_read_ + raw_msg;
          auto total_bytes_transferred = self->prev_read_bytes_ + bytes_transferred;
          full_raw_msg.resize(total_bytes_transferred);

          if (encryption::is_encrypted(full_raw_msg)) { // The header states the full length, see rtsp/encryption.hpp
            auto total_length = encryption::full_message_size(full_raw_msg);
            if (total_length && *total_length > max_encrypted_msg_size) {
              logs::log(logs::warning, "[RTSP] encrypted message too big: {} bytes", *total_length);
              return finish(std::nullopt);
            }
            if (!total_length || total_bytes_transferred < *total_length) {
              if (timed_out) {
                logs::log(logs::warning, "[RTSP] timed out waiting for the rest of an encrypted message");
                return finish(std::nullopt);
              }
              self->prev_read_ = full_raw_msg;
              self->prev_read_bytes_ += bytes_transferred;
              return self->receive_raw(on_msg_read);
            }
            return finish(full_raw_msg);
          }

          // We should be able to rtsp::parse() even when getting a partial message
          // Since we had a few issues with this approach in the past now I'm manually reading the "Content-length"
          // It's ugly, but it shouldn't be slower than parsing the whole message.
          auto content_length_pos = full_raw_msg.find("Content-length: ");
          if (content_length_pos != std::string::npos) {
            content_length_pos += 16; //"Content-length: "sv.size();
            auto content_lenght_end = full_raw_msg.find("\r\n", content_length_pos);
            if (content_lenght_end == std::string::npos) {
              if (timed_out || total_bytes_transferred >= max_total_msg_size) {
                logs::log(logs::warning, "[RTSP] gave up waiting for the end of the Content-length header");
                return finish(std::nullopt);
              }
              // The header line itself was split across reads, wait for the rest of it
              self->prev_read_ = full_raw_msg;
              self->prev_read_bytes_ += bytes_transferred;
              return self->receive_raw(on_msg_read);
            }
            auto total_length_str = full_raw_msg.substr(content_length_pos, content_lenght_end - content_length_pos);
            // The header comes from an unauthenticated peer: reject anything that isn't a plain number, or that
            // would make us buffer more than max_total_msg_size, instead of throwing or waiting on it forever.
            auto content_length = utils::to_size_t(total_length_str);
            if (!content_length || *content_length > max_total_msg_size) {
              logs::log(logs::warning, "[RTSP] invalid Content-length: {}", total_length_str);
              return finish(std::nullopt);
            }
            auto total_length = *content_length + content_lenght_end + 2; // 2 for the \r\n
            if (total_bytes_transferred < total_length) {
              if (timed_out) { // the payload didn't arrive in time
                logs::log(logs::warning, "[RTSP] timed out waiting for the message payload");
                return finish(std::nullopt);
              }
              self->prev_read_ = full_raw_msg;
              self->prev_read_bytes_ += bytes_transferred;
              return self->receive_raw(on_msg_read);
            }
          }

          finish(full_raw_msg);
        });
  }

  /**
   * Will fully write back the given message to the socket
   * calls on_sent(bytes_transferred) when over
   */
  void send_message(const rtsp::RTSP_PACKET &response,
                    const std::function<void(int /* bytes_transferred */)> &on_sent) {
    auto raw_response = rtsp::to_string(response);
    logs::log(logs::trace, "[RTSP] sending reply: \n{}", raw_response);
    send_raw(std::move(raw_response), on_sent);
  }

  void send_raw(std::string raw_msg, const std::function<void(int /* bytes_transferred */)> &on_sent) {
    // async_write returns before the bytes are sent: the handler owns the buffer until it's done
    auto buffer = std::make_shared<std::string>(std::move(raw_msg));
    boost::asio::async_write(socket(),
                             boost::asio::buffer(*buffer),
                             [on_sent, buffer](auto error_code, auto bytes_transferred) {
                               if (error_code) {
                                 logs::log(logs::error, "[RTSP] error during transmission: {}", error_code.message());
                               }
                               logs::log(logs::trace, "[RTSP] sent reply of size: {}", bytes_transferred);
                               on_sent(bytes_transferred);
                             });
  }

protected:
  explicit tcp_connection(boost::asio::io_context &io_context, state::SessionsAtoms stream_sessions)
      : socket_(io_context), streambuf_(max_msg_size), deadline_(io_context),
        stream_sessions(std::move(stream_sessions)), prev_read_bytes_(0) {}
  tcp::socket socket_;

  /**
   * Sequence number of the last encrypted RTSP message we sent: it's the GCM IV, so it must never repeat under one key.
   * Process wide, not per session: two sessions may share a key (an API caller or a /resume reusing a rikey).
   */
  static inline std::atomic<std::uint32_t> encryption_seq_ = 0;

  static constexpr auto max_msg_size = 2048;
  /** Upper bound on a message's declared Content-length; Moonlight's largest (ANNOUNCE) is a few KiB */
  static constexpr std::size_t max_total_msg_size = 64 * 1024;
  // Moonlight's largest message (ANNOUNCE) is ~1KB; this also bounds the stack buffer AES-GCM decrypts into
  static constexpr std::size_t max_encrypted_msg_size = 16 * 1024;
  static constexpr auto timeout_millis = 2500;

  boost::asio::streambuf streambuf_;
  asio::steady_timer deadline_;
  std::string prev_read_;
  int prev_read_bytes_;

  state::SessionsAtoms stream_sessions;
};

/**
 * A generic TCP server that accepts incoming client connections
 * adapted from the tutorial at: https://www.boost.org/doc/libs/1_79_0/doc/html/boost_asio/tutorial/tutdaytime3.html
 */
class tcp_server {
public:
  tcp_server(boost::asio::io_context &io_context, int port, state::SessionsAtoms state)
      : io_context_(io_context), acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
        stream_sessions(std::move(state)) {
    acceptor_.set_option(boost::asio::socket_base::reuse_address{true});
    acceptor_.listen(4096);
    start_accept();
  }

private:
  /**
   * Creates a socket and initiates an asynchronous accept operation to wait for a new connection
   */
  void start_accept() {
    tcp_connection::pointer new_connection = tcp_connection::create(io_context_, stream_sessions);

    acceptor_.async_accept(new_connection->socket(),
                           [this, new_connection](auto error) { handle_accept(new_connection, error); });
  }

  /**
   * Called when the asynchronous accept operation initiated by start_accept() finishes. It services the client
   * request, and then calls start_accept() to initiate the next accept operation.
   */
  void handle_accept(const tcp_connection::pointer &new_connection, const boost::system::error_code &error) {
    // Re-arm the acceptor FIRST, so that anything going wrong while servicing
    // this connection (e.g. the peer reset and a socket op throws) cannot stop
    // the server from accepting future connections.
    start_accept();
    if (!error) {
      try {
        new_connection->start();
      } catch (const std::exception &e) {
        logs::log(logs::warning, "[RTSP] error servicing connection: {}", e.what());
      }
    } else {
      logs::log(logs::error, "[RTSP] error during connection: {}", error.message());
    }
  }

  boost::asio::io_context &io_context_;
  tcp::acceptor acceptor_;
  state::SessionsAtoms stream_sessions;
};

/**
 * Starts a new RTSP server, calling this method will block execution.
 */
void run_server(int port, const state::SessionsAtoms &running_sessions) {
  boost::asio::io_context io_context;
  try {
    tcp_server server(io_context, port, running_sessions);

    logs::log(logs::info, "RTSP server started on port: {}", port);

    // Block here processing connections until the context is stopped. A single
    // misbehaving connection handler must never take the whole server down: if
    // one throws, log it and resume the accept loop (the acceptor is still
    // alive) instead of letting the exception unwind run_server and leave the
    // RTSP port dead until Wolf restarts.
    while (true) {
      try {
        io_context.run();
        break; // clean stop: no more work queued
      } catch (const std::exception &e) {
        logs::log(logs::warning, "[RTSP] connection handler exception: {} (resuming)", e.what());
      }
    }
  } catch (std::exception &e) {
    logs::log(logs::error, "Unable to create RTSP server on port: {} ex: {}", port, e.what());
    throw; // Wolf can't stream without it: let the caller shut down
  }
}

} // namespace rtsp
