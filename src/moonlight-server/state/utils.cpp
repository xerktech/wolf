#include <cerrno>
#include <curl/curl.h>
#include <fcntl.h>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <state/utils.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace utils {

/* Icons are small: anything bigger is refused instead of being buffered in memory (e.g. /dev/zero) */
constexpr std::size_t MAX_ASSET_SIZE = 10 * 1024 * 1024;

std::optional<std::string> curl_get(std::string_view url) {
  auto curl = curl_easy_init();
  auto url_str = utils::to_string(url);
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
    /* Set custom writer (in order to receive back the response) */
    curl_easy_setopt(curl,
                     CURLOPT_WRITEFUNCTION,
                     static_cast<size_t (*)(char *, size_t, size_t, void *)>(
                         [](char *ptr, size_t size, size_t nmemb, void *read_buf) {
                           auto buf = static_cast<std::string *>(read_buf);
                           if (buf->size() + size * nmemb > MAX_ASSET_SIZE) {
                             return std::size_t{0}; // Aborts the transfer with CURLE_WRITE_ERROR
                           }
                           *buf += std::string{ptr, size * nmemb};
                           return size * nmemb;
                         }));
    std::string read_buf;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    auto res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      logs::log(logs::warning, "Could not download asset from {}, {}", url, curl_easy_strerror(res));
    } else {
      long response_code;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
      if (response_code == 200) {
        curl_easy_cleanup(curl);
        return read_buf;
      } else {
        logs::log(logs::warning, "Could not download asset from {}, response code: {}", url, response_code);
      }
    }
  }
  curl_easy_cleanup(curl);
  return {};
}

std::optional<std::string> get_file_content(const std::filesystem::path &path) {
  // Only regular files: directories throw while reading, FIFOs block forever, devices never end.
  // Check the path first so that we never open() anything else (opening a FIFO or a device has side effects),
  // then check the opened fd too so that the file can't be swapped between the two;
  // O_NONBLOCK keeps the open itself from blocking on a FIFO swapped in meanwhile.
  struct stat path_st {};
  if (::stat(path.c_str(), &path_st) != 0 || !S_ISREG(path_st.st_mode)) {
    return {};
  }
  int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) {
    return {};
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::size_t>(st.st_size) > MAX_ASSET_SIZE) {
    ::close(fd);
    return {};
  }
  std::string content(static_cast<std::size_t>(st.st_size), '\0');
  std::size_t total = 0;
  while (total < content.size()) {
    auto n = ::read(fd, content.data() + total, content.size() - total);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    total += static_cast<std::size_t>(n);
  }
  ::close(fd);
  content.resize(total);
  return content;
}

/**
 * Returns the position of the `>` closing the `<!DOCTYPE` that `doc` starts with, npos if there's none.
 * Quoted literals (`SYSTEM "a[b>"`) are skipped, and so is an internal subset (`[ <!ENTITY ...> ]`, e.g.
 * Illustrator exports) together with the quoted values, comments and processing instructions inside it, since all of
 * them can hold quotes, `[`, `]` or `>` of their own.
 */
static std::size_t doctype_end(std::string_view doc) {
  using namespace std::string_view_literals;
  bool in_subset = false;
  for (std::size_t i = "<!DOCTYPE"sv.size(); i < doc.size(); i++) {
    if (doc[i] == '"' || doc[i] == '\'') {
      i = doc.find(doc[i], i + 1);
    } else if (in_subset && doc.substr(i).starts_with("<!--"sv)) {
      i = doc.find("-->"sv, i + 4);
      i = i == std::string_view::npos ? i : i + 2;
    } else if (in_subset && doc.substr(i).starts_with("<?"sv)) {
      i = doc.find("?>"sv, i + 2);
      i = i == std::string_view::npos ? i : i + 1;
    } else if (doc[i] == '[' && !in_subset) {
      in_subset = true;
    } else if (doc[i] == ']' && in_subset) {
      in_subset = false;
    } else if (doc[i] == '>' && !in_subset) {
      return i;
    }
    if (i == std::string_view::npos) {
      break;
    }
  }
  return std::string_view::npos;
}

std::optional<std::string> image_mime_type(std::string_view content) {
  using namespace std::string_view_literals;
  if (content.starts_with("\x89PNG\r\n\x1a\n"sv)) {
    return "image/png";
  }
  if (content.starts_with("\xff\xd8\xff"sv)) {
    return "image/jpeg";
  }
  if (content.starts_with("GIF87a"sv) || content.starts_with("GIF89a"sv)) {
    return "image/gif";
  }
  if (content.size() >= 12 && content.starts_with("RIFF"sv) && content.substr(8, 4) == "WEBP"sv) {
    return "image/webp";
  }
  if (content.starts_with("\x00\x00\x01\x00"sv)) {
    return "image/x-icon";
  }
  // SVG is text: the root element must be <svg, optionally preceded (within the first 1 KB) by a BOM,
  // whitespace, an XML declaration, comments and a DOCTYPE. Never accept `<svg` found elsewhere in a document.
  auto head = content.substr(0, 1024);
  if (head.starts_with("\xef\xbb\xbf"sv)) { // UTF-8 BOM
    head.remove_prefix(3);
  }
  while (true) {
    auto first = head.find_first_not_of(" \t\r\n"sv);
    if (first == std::string_view::npos) {
      return std::nullopt;
    }
    head.remove_prefix(first);
    std::string_view opening, closing;
    if (head.starts_with("<?"sv)) {
      opening = "<?"sv;
      closing = "?>"sv;
    } else if (head.starts_with("<!--"sv)) {
      opening = "<!--"sv;
      closing = "-->"sv;
    } else if (head.starts_with("<!DOCTYPE"sv)) {
      auto end = doctype_end(head);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      head.remove_prefix(end + 1);
      continue;
    } else {
      break;
    }
    auto end = head.find(closing, opening.size());
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    head.remove_prefix(end + closing.size());
  }
  // `<svg` must be the whole tag name: `<svg>`, `<svg/>` or `<svg attr...`, never `<svg-foo`
  if (head.size() > 4 && head.starts_with("<svg"sv) && " \t\r\n>/"sv.find(head[4]) != std::string_view::npos) {
    return "image/svg+xml";
  }
  return std::nullopt;
}

static std::optional<std::string> only_images(std::optional<std::string> content, std::string_view source) {
  if (content && !image_mime_type(*content)) {
    logs::log(logs::warning, "Refusing to serve configured asset, not a supported image: {}", source);
    return std::nullopt;
  }
  return content;
}

std::optional<std::string> get_icon(std::string_view base_local_path, std::string_view icon_path) {
  if (icon_path.starts_with("/")) { // Absolute path
    auto asset_path = std::filesystem::path(icon_path);
    if (auto file_content = utils::get_file_content(asset_path)) {
      return only_images(file_content, icon_path);
    }
    logs::log(logs::warning, "Could not open configured asset: {}", asset_path.string());
  } else if (icon_path.starts_with("http")) { // URL
    if (auto file_content = utils::curl_get(icon_path)) {
      return only_images(file_content, icon_path);
    }
    logs::log(logs::warning, "Could not download configured asset: {}", icon_path);
  } else { // Assume it's a relative path (legacy setting)
    auto asset_path = std::filesystem::path(base_local_path) / icon_path;
    if (auto file_content = utils::get_file_content(asset_path)) {
      return only_images(file_content, asset_path.string());
    }
    logs::log(logs::warning, "Could not open configured asset: {}", asset_path.string());
  }

  return std::nullopt;
}

} // namespace utils