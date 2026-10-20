#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace utils {

std::optional<std::string> curl_get(std::string_view url);

std::optional<std::string> get_file_content(const std::filesystem::path &path);

/**
 * Returns the MIME type of `content` if it starts with the signature of a supported image format
 * (PNG, JPEG, GIF, WebP, ICO, SVG), std::nullopt otherwise.
 * Detection is by content (magic bytes), never by file extension.
 */
std::optional<std::string> image_mime_type(std::string_view content);

/**
 * Returns the raw content of the icon (if available).
 *
 * icon_path can be a URL, a relative or absolute path.
 * Only content recognised by image_mime_type() is returned: icon_path can be supplied by API clients,
 * so this must never become a way to read arbitrary files (private keys, config, /etc/shadow, ...).
 */
std::optional<std::string> get_icon(std::string_view base_local_path, std::string_view icon_path);

} // namespace utils