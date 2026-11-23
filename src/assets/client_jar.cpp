#include "assets/client_jar.hpp"

#include <curl/curl.h>
#include <openssl/sha.h>
#include <simdjson.h>

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace ds {

namespace fs = std::filesystem;

namespace {

std::string to_hex_lower(const u8* digest, size_t len) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(kHex[(digest[i] >> 4) & 0xf]);
    out.push_back(kHex[digest[i] & 0xf]);
  }
  return out;
}

std::string to_lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool is_alias_version(std::string_view v) {
  return v.empty() || v == "latest" || v == "release" || v == "snapshot";
}

fs::path cache_dir() {
  fs::path cache;
  if (const char* x = std::getenv("XDG_CACHE_HOME")) cache = x;
  else if (const char* h = std::getenv("HOME")) cache = fs::path(h) / ".cache";
  else cache = fs::temp_directory_path();
  return cache / "deepslate-edition/clients";
}

void configure_curl_common(CURL* c, const std::string& url) {
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "deepslate-edition/0.1");
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
  if (const char* ca = std::getenv("CURL_CA_BUNDLE")) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
  else if (const char* ca2 = std::getenv("SSL_CERT_FILE")) curl_easy_setopt(c, CURLOPT_CAINFO, ca2);
}

size_t write_string_cb(char* p, size_t sz, size_t n, void* ud) {
  auto* out = static_cast<std::string*>(ud);
  out->append(p, sz * n);
  return sz * n;
}

struct FileWriteState {
  std::ofstream* file = nullptr;
  SHA_CTX sha{};
  bool write_ok = true;
};

size_t write_file_sha_cb(char* p, size_t sz, size_t n, void* ud) {
  auto* st = static_cast<FileWriteState*>(ud);
  size_t bytes = sz * n;
  if (!st->file->write(p, static_cast<std::streamsize>(bytes))) {
    st->write_ok = false;
    return 0;
  }
  SHA1_Update(&st->sha, p, bytes);
  return bytes;
}

std::optional<std::string> http_get_string(const std::string& url, std::string* err) {
  CURL* c = curl_easy_init();
  if (!c) {
    if (err) *err = "curl init failed";
    return std::nullopt;
  }
  std::string body;
  configure_curl_common(c, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_string_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
  CURLcode rc = curl_easy_perform(c);
  curl_easy_cleanup(c);
  if (rc != CURLE_OK) {
    if (err) *err = std::string("download failed (") + url + "): " + curl_easy_strerror(rc);
    return std::nullopt;
  }
  return body;
}

bool http_download_verified(const std::string& url, const fs::path& dest,
                            std::string_view expected_sha1, std::string* err) {
  std::error_code ec;
  if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
  if (ec) {
    if (err) *err = "cannot create directory: " + ec.message();
    return false;
  }

  fs::path tmp = dest;
  tmp += ".part";
  CURL* c = curl_easy_init();
  if (!c) {
    if (err) *err = "curl init failed";
    return false;
  }

  FileWriteState st;
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) {
    curl_easy_cleanup(c);
    if (err) *err = "cannot open output file: " + tmp.string();
    return false;
  }
  st.file = &out;
  SHA1_Init(&st.sha);

  configure_curl_common(c, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_file_sha_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &st);
  CURLcode rc = curl_easy_perform(c);
  curl_easy_cleanup(c);
  out.close();

  if (rc != CURLE_OK || !st.write_ok) {
    fs::remove(tmp, ec);
    if (err) {
      *err = !st.write_ok ? "failed writing downloaded file"
                          : std::string("download failed (") + url + "): " + curl_easy_strerror(rc);
    }
    return false;
  }

  u8 digest[SHA_DIGEST_LENGTH];
  SHA1_Final(digest, &st.sha);
  std::string actual_sha1 = to_hex_lower(digest, sizeof(digest));
  if (!expected_sha1.empty() && actual_sha1 != to_lower_ascii(expected_sha1)) {
    fs::remove(tmp, ec);
    if (err) {
      *err = std::format("SHA-1 mismatch for {}: expected {}, got {}", url, expected_sha1, actual_sha1);
    }
    return false;
  }

  fs::rename(tmp, dest, ec);
  if (ec) {
    fs::remove(tmp, ec);
    if (err) *err = "cannot move downloaded file to " + dest.string() + ": " + ec.message();
    return false;
  }
  return true;
}

std::optional<std::pair<ManifestVersion, ClientDownloadInfo>> resolve_client_download(
    std::string_view version, std::string_view manifest_url, std::string* err) {
  log_info("fetching version manifest {}", manifest_url);
  auto manifest_json = http_get_string(std::string(manifest_url), err);
  if (!manifest_json) return std::nullopt;

  auto mv = parse_version_manifest(*manifest_json, version, err);
  if (!mv) return std::nullopt;

  log_info("fetching metadata for Minecraft {} ({})", mv->id, mv->url);
  auto version_json = http_get_string(mv->url, err);
  if (!version_json) return std::nullopt;

  auto info = parse_client_download(*version_json, err);
  if (!info) return std::nullopt;
  return std::make_pair(std::move(*mv), std::move(*info));
}

}  // namespace

std::string sha1_hex(std::span<const u8> data) {
  u8 digest[SHA_DIGEST_LENGTH];
  SHA1(data.data(), data.size(), digest);
  return to_hex_lower(digest, sizeof(digest));
}

std::optional<std::string> sha1_file_hex(const fs::path& path, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = "cannot open file: " + path.string();
    return std::nullopt;
  }
  SHA_CTX ctx;
  SHA1_Init(&ctx);
  char buf[65536];
  while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
    SHA1_Update(&ctx, buf, static_cast<size_t>(in.gcount()));
  }
  if (in.bad()) {
    if (err) *err = "error reading file: " + path.string();
    return std::nullopt;
  }
  u8 digest[SHA_DIGEST_LENGTH];
  SHA1_Final(digest, &ctx);
  return to_hex_lower(digest, sizeof(digest));
}

std::optional<ManifestVersion> parse_version_manifest(std::string_view json,
                                                      std::string_view version,
                                                      std::string* err) {
  simdjson::dom::parser parser;
  simdjson::padded_string ps(json.data(), json.size());
  simdjson::dom::element doc;
  if (auto ec = parser.parse(ps).get(doc); ec != simdjson::SUCCESS) {
    if (err) *err = std::string("invalid manifest JSON: ") + simdjson::error_message(ec);
    return std::nullopt;
  }

  std::string_view target_id = version;
  if (version.empty() || version == "latest" || version == "release") {
    if (auto ec = doc["latest"]["release"].get(target_id); ec != simdjson::SUCCESS) {
      if (err) *err = std::string("missing latest.release in manifest: ") + simdjson::error_message(ec);
      return std::nullopt;
    }
  } else if (version == "snapshot") {
    if (auto ec = doc["latest"]["snapshot"].get(target_id); ec != simdjson::SUCCESS) {
      if (err) *err = std::string("missing latest.snapshot in manifest: ") + simdjson::error_message(ec);
      return std::nullopt;
    }
  }

  simdjson::dom::array versions;
  if (auto ec = doc["versions"].get(versions); ec != simdjson::SUCCESS) {
    if (err) *err = std::string("missing versions array in manifest: ") + simdjson::error_message(ec);
    return std::nullopt;
  }

  for (simdjson::dom::element entry : versions) {
    std::string_view id;
    if (entry["id"].get(id) != simdjson::SUCCESS || id != target_id) continue;

    std::string_view url;
    if (auto ec = entry["url"].get(url); ec != simdjson::SUCCESS) {
      if (err) *err = std::string("missing url for version ") + std::string(id) + ": " + simdjson::error_message(ec);
      return std::nullopt;
    }
    ManifestVersion mv;
    mv.id = std::string(id);
    mv.url = std::string(url);
    std::string_view sv;
    if (entry["type"].get(sv) == simdjson::SUCCESS) mv.type = std::string(sv);
    if (entry["sha1"].get(sv) == simdjson::SUCCESS) mv.sha1 = std::string(sv);
    return mv;
  }

  if (err) *err = "version not found in manifest: " + std::string(target_id);
  return std::nullopt;
}

std::optional<ClientDownloadInfo> parse_client_download(std::string_view json, std::string* err) {
  simdjson::dom::parser parser;
  simdjson::padded_string ps(json.data(), json.size());
  simdjson::dom::element doc;
  if (auto ec = parser.parse(ps).get(doc); ec != simdjson::SUCCESS) {
    if (err) *err = std::string("invalid version JSON: ") + simdjson::error_message(ec);
    return std::nullopt;
  }

  simdjson::dom::object client;
  if (auto ec = doc["downloads"]["client"].get(client); ec != simdjson::SUCCESS) {
    if (err) *err = std::string("missing downloads.client in version JSON: ") + simdjson::error_message(ec);
    return std::nullopt;
  }

  std::string_view url;
  if (auto ec = client["url"].get(url); ec != simdjson::SUCCESS) {
    if (err) *err = std::string("missing downloads.client.url in version JSON: ") + simdjson::error_message(ec);
    return std::nullopt;
  }

  ClientDownloadInfo info;
  info.url = std::string(url);
  std::string_view sha1;
  if (client["sha1"].get(sha1) == simdjson::SUCCESS) info.sha1 = std::string(sha1);
  u64 size = 0;
  if (client["size"].get(size) == simdjson::SUCCESS) info.size = size;
  return info;
}

std::optional<std::string> download_client_jar(std::string_view version, std::string* err,
                                               std::string_view manifest_url) {
  fs::path dir = cache_dir();
  if (!is_alias_version(version)) {
    fs::path cached = dir / (std::string(version) + ".jar");
    std::error_code ec;
    if (fs::is_regular_file(cached, ec) && fs::file_size(cached, ec) > 0) {
      log_info("using cached client.jar {}", cached.string());
      return cached.string();
    }
  }

  auto resolved = resolve_client_download(version, manifest_url, err);
  if (!resolved) return std::nullopt;
  const auto& [mv, info] = *resolved;

  fs::path target = dir / (mv.id + ".jar");
  std::error_code ec;
  if (fs::is_regular_file(target, ec)) {
    if (info.sha1.empty()) {
      log_info("using cached client.jar {}", target.string());
      return target.string();
    }
    auto actual = sha1_file_hex(target, nullptr);
    if (actual && *actual == to_lower_ascii(info.sha1)) {
      log_info("using cached client.jar {}", target.string());
      return target.string();
    }
  }

  log_info("downloading Minecraft {} client.jar ({})", mv.id, info.url);
  if (!http_download_verified(info.url, target, info.sha1, err)) return std::nullopt;
  return target.string();
}

bool download_client_jar_to(std::string_view version, const fs::path& dest_path, std::string* err,
                            std::string_view manifest_url) {
  auto resolved = resolve_client_download(version, manifest_url, err);
  if (!resolved) return false;
  const auto& [mv, info] = *resolved;

  std::error_code ec;
  if (!info.sha1.empty() && fs::is_regular_file(dest_path, ec)) {
    auto actual = sha1_file_hex(dest_path, nullptr);
    if (actual && *actual == to_lower_ascii(info.sha1)) {
      log_info("client.jar already up to date at {}", dest_path.string());
      return true;
    }
  }

  log_info("downloading Minecraft {} client.jar to {}", mv.id, dest_path.string());
  return http_download_verified(info.url, dest_path, info.sha1, err);
}

}  // namespace ds
