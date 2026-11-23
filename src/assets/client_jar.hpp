#pragma once
// Downloads Minecraft Java Edition client.jar using Mojang's version_manifest_v2.json.
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "util/common.hpp"

namespace ds {

inline constexpr std::string_view kDefaultVersionManifestUrl =
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

struct ManifestVersion {
  std::string id;
  std::string type;
  std::string url;
  std::string sha1;
};

struct ClientDownloadInfo {
  std::string url;
  std::string sha1;
  u64 size = 0;
};

// Resolves `version` ("", "latest", "release", "snapshot", or an explicit version id like "26.3")
// from version_manifest_v2.json using simdjson.
std::optional<ManifestVersion> parse_version_manifest(std::string_view json,
                                                      std::string_view version,
                                                      std::string* err);

// Extracts downloads.client.{url, sha1, size} from a version metadata JSON using simdjson.
std::optional<ClientDownloadInfo> parse_client_download(std::string_view json, std::string* err);

// Computes lowercase hex SHA-1 of `data`.
std::string sha1_hex(std::span<const u8> data);

// Computes lowercase hex SHA-1 of a file on disk.
std::optional<std::string> sha1_file_hex(const std::filesystem::path& path, std::string* err);

// Downloads (or returns cached) client.jar for `version` ("latest", "release", "snapshot", or
// explicit id like "26.3") into the cache directory (~/.cache/deepslate-edition/clients/<id>.jar).
// Verifies SHA-1 when available.
std::optional<std::string> download_client_jar(
    std::string_view version, std::string* err,
    std::string_view manifest_url = kDefaultVersionManifestUrl);

// Downloads client.jar for `version` to `dest_path` (creating parent dirs and verifying SHA-1).
bool download_client_jar_to(std::string_view version, const std::filesystem::path& dest_path,
                            std::string* err,
                            std::string_view manifest_url = kDefaultVersionManifestUrl);

}  // namespace ds
