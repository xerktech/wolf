#pragma once
#include <helpers/logger.hpp>
#include <map>
#include <optional>
#include <peglib.h>
#include <sstream>
#include <vector>

namespace rtsp {

enum PACKET_TYPE {
  REQUEST,
  RESPONSE
};

enum TARGET_TYPE {
  TARGET_URI,
  TARGET_STREAM
};

struct URI {
  std::string protocol;
  std::string ip; // Do not rely on this, can be missing (AndroidTV)
  unsigned short port{};
};

struct STREAM {
  std::string type;
  std::string params;
};

struct RTSP_REQUEST {
  std::string cmd;

  TARGET_TYPE type{};

  URI uri;
  STREAM stream;
};

struct RTSP_RESPONSE {
  unsigned short status_code{};
  std::string msg;
};

struct RTSP_PACKET {
  PACKET_TYPE type{};

  int seq_number{};

  RTSP_REQUEST request;
  RTSP_RESPONSE response;

  struct response;

  std::map<std::string, std::string> options = {};
  std::vector<std::pair<std::string, std::string>> payloads = {};
};

namespace detail {

/** The message being parsed on this thread, for the grammar's error logger */
inline thread_local std::string_view current_msg;

inline RTSP_PACKET &packet(std::any &dt) {
  return *std::any_cast<RTSP_PACKET *>(dt);
}

/**
 * Builds the RTSP grammar and its actions.
 * Compiling the grammar is expensive (it dominated the single RTSP thread under a flood of connections),
 * so it's done once per thread; the actions write into the packet passed as parse()'s user data.
 */
inline peg::parser make_parser() {
  // clang-format off
  // Test this out at https://yhirose.github.io/cpp-peglib/
  peg::parser parser = {R"(
    RTSP <- (RTSPREQUEST / RTSPRESPONSE) ENDLINE CSEQ OPTION* ENDLINE? PAYLOAD* RUBBISH?

    RTSPREQUEST <- CMD TARGET FULLPROTOCOL
    RTSPRESPONSE <- FULLPROTOCOL RESPONSECODE RESPONSEMSG

    #####
    CMD <- < [a-z]i+ >

    #####
    RESPONSECODE <- < [0-9]+ >
    RESPONSEMSG <- < [a-zA-Z ]+ >

    #####
    TARGET <- URI / STREAM / '/'
    STREAM <- [a-z]i+ '=' STREAMTYPE STREAMPARAMS

    STREAMTYPE <- < [a-z]i+  >
    STREAMPARAMS <- < [0-9/]+ >

    URI <- PROTOCOL '://' IP? ':' PORT
    PROTOCOL <- < [a-z]i+ >
    IP <-  < [0-9.]+ >
    PORT <- < [0-9]+ >

    #####
    ~FULLPROTOCOL <- FPROTOCOL '/' FVERSION
    FPROTOCOL <- < [a-z]i+ >
    FVERSION <- < [0-9.]+ >

    #####
    CSEQ <- 'CSeq:' < [0-9.]+ > ENDLINE?

    #####
    OPTION <- OPTKEY ':' OPTVAL ENDLINE?
    OPTKEY <- < [a-zA-Z0-9-_]+ >
    OPTVAL <- < [a-zA-Z0-9-_./;=, :]+ > / ''

    #####
    PAYLOAD <- PAYLOADKEY '=' PAYLOADVAL ENDLINE?
    PAYLOADKEY <- < [a-zA-Z0-9-_]+ >
    PAYLOADVAL <- < [a-zA-Z0-9-_./[\]\\:  =]+ > / ''

    #####
    CR <- '\r'
    LF <- '\n'
    ~ENDLINE <- CR? LF
    RUBBISH <- < .+ > # Any unicode character

    %whitespace  <-  [ \t]*
    )"};
  // clang-format on

  if (!static_cast<bool>(parser)) { // If this fails we have passed a bad grammar
    return parser;
  }

  parser["RTSPREQUEST"] = [](const peg::SemanticValues &vs, std::any &dt) { packet(dt).type = REQUEST; };
  parser["RTSPRESPONSE"] = [](const peg::SemanticValues &vs, std::any &dt) { packet(dt).type = RESPONSE; };

  parser["RESPONSECODE"] = [](const peg::SemanticValues &vs, std::any &dt) {
    packet(dt).response.status_code = vs.token_to_number<unsigned short>();
  };
  parser["RESPONSEMSG"] = [](const peg::SemanticValues &vs, std::any &dt) { packet(dt).response.msg = vs.token(); };

  parser["CMD"] = [](const peg::SemanticValues &vs, std::any &dt) { packet(dt).request.cmd = vs.token(); };
  parser["CSEQ"] = [](const peg::SemanticValues &vs, std::any &dt) {
    packet(dt).seq_number = vs.token_to_number<int>();
  };

  // Target = STREAM
  parser["STREAM"] = [](const peg::SemanticValues &vs, std::any &dt) { packet(dt).request.type = TARGET_STREAM; };
  parser["STREAMTYPE"] = [](const peg::SemanticValues &vs, std::any &dt) {
    packet(dt).request.stream.type = vs.token();
  };
  parser["STREAMPARAMS"] = [](const peg::SemanticValues &vs, std::any &dt) {
    packet(dt).request.stream.params = vs.token();
  };

  // Target = URI
  parser["URI"] = [](const peg::SemanticValues &vs, std::any &dt) { packet(dt).request.type = TARGET_URI; };
  parser["PROTOCOL"] = [](const peg::SemanticValues &vs, std::any &dt) {
    packet(dt).request.uri.protocol = vs.token();
  };
  parser["IP"] = [](const peg::SemanticValues &vs, std::any &dt) { packet(dt).request.uri.ip = vs.token(); };
  parser["PORT"] = [](const peg::SemanticValues &vs, std::any &dt) {
    packet(dt).request.uri.port = vs.token_to_number<unsigned short>();
  };

  // Options map
  parser["OPTION"] = [](const peg::SemanticValues &vs, std::any &dt) {
    auto key = std::any_cast<std::string>(vs[0]);
    auto val = std::any_cast<std::string>(vs[1]);
    packet(dt).options[key] = val;
  };
  parser["OPTKEY"] = [](const peg::SemanticValues &vs) { return vs.token_to_string(); };
  parser["OPTVAL"] = [](const peg::SemanticValues &vs) { return vs.token_to_string(); };

  // Payloads map
  parser["PAYLOAD"] = [](const peg::SemanticValues &vs, std::any &dt) {
    auto key = std::any_cast<std::string>(vs[0]);
    auto val = std::any_cast<std::string>(vs[1]);
    packet(dt).payloads.emplace_back(key, val);
  };
  parser["PAYLOADKEY"] = [](const peg::SemanticValues &vs) { return vs.token_to_string(); };
  parser["PAYLOADVAL"] = [](const peg::SemanticValues &vs) { return vs.token_to_string(); };

  parser.set_logger([](size_t line, size_t col, const std::string &error_msg, const std::string &rule) {
    logs::log(logs::warning, "RTSP - {}:{}: {}\n{}", line, col, error_msg, current_msg);
  });

  parser.enable_packrat_parsing();
  return parser;
}

} // namespace detail

/**
 * Parse the input message; if successful will return a PACKET object.
 */
inline std::optional<RTSP_PACKET> parse(std::string_view msg) {
  thread_local peg::parser parser = detail::make_parser();
  if (!static_cast<bool>(parser)) { // If this fails we have passed a bad grammar
    return {};
  }

  RTSP_PACKET pkt;
  std::any dt = &pkt;
  detail::current_msg = msg;
  bool parsed = parser.parse(msg, dt);
  detail::current_msg = {};
  if (!parsed) { // If this fails we have passed a packet that doesn't conform to the grammar
    return {};
  }
  return pkt;
}

/**
 * Turns the packet into a string, ready to be fired down the UDP channel
 */
std::string to_string(const RTSP_PACKET &pkt) {
  std::ostringstream stream;
  constexpr auto endl = "\r\n";

  if (pkt.type == REQUEST) {
    stream << pkt.request.cmd << " ";

    if (pkt.request.type == TARGET_URI) {
      stream << pkt.request.uri.protocol << "://" << pkt.request.uri.ip << ":" << pkt.request.uri.port;
    } else {
      stream << "streamid=" << pkt.request.stream.type << pkt.request.stream.params;
    }

    stream << " RTSP/1.0";
  } else {
    stream << "RTSP/1.0 " << pkt.response.status_code << " " << pkt.response.msg;
  }

  stream << endl << "CSeq: " << pkt.seq_number;

  for (const auto &opt : pkt.options) {
    stream << "\r\n" << opt.first << ": " << opt.second;
  }
  stream << endl << endl;
  for (const auto &pay : pkt.payloads) {
    if (pay.first.empty()) {
      stream << pay.second << endl;
    } else {
      stream << pay.first << "=" << pay.second << endl;
    }
  }

  return stream.str();
}

} // namespace rtsp
