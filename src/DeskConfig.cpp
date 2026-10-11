#include "DeskConfig.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <strings.h>

#include <algorithm>
#include <utility>

namespace {
// URL schemes are case-insensitive (RFC 3986); the HTTP client lowercases them when parsing.
bool isHttpUrl(const std::string& url) {
  return strncasecmp(url.c_str(), "http://", 7) == 0 || strncasecmp(url.c_str(), "https://", 8) == 0;
}
}  // namespace

bool DeskConfig::fromJson(JsonVariantConst root, DeskConfig& out, std::string& error) {
  // Read strings as const char* (see PersistableStore.h) to avoid a second copy of the JSON serializer.
  DeskConfig cfg;
  cfg.imageUrl = root["image_url"] | "";
  cfg.versionUrl = root["version_url"] | "";
  cfg.webhookUrl = root["webhook_url"] | "";
  cfg.pollS = std::clamp(root["poll_s"] | DEFAULT_POLL_S, MIN_POLL_S, MAX_POLL_S);
  cfg.fullRefreshEvery = std::max(root["full_refresh_every"] | DEFAULT_FULL_REFRESH_EVERY, 1);

  if (!isHttpUrl(cfg.imageUrl)) {
    error = "image_url missing or not http(s)";
    return false;
  }
  if (!cfg.versionUrl.empty() && !isHttpUrl(cfg.versionUrl)) {
    error = "version_url must be http(s)";
    return false;
  }
  if (!isHttpUrl(cfg.webhookUrl)) {
    error = "webhook_url missing or not http(s)";
    return false;
  }

  out = std::move(cfg);
  return true;
}

bool DeskConfig::load(DeskConfig& out, std::string& error) {
  const String text = Storage.readFile(PATH);
  if (text.isEmpty()) {
    error = "missing or empty";
    LOG_ERR("DESK", "%s: %s", PATH, error.c_str());
    return false;
  }
  // Editors on Windows often save a UTF-8 byte order mark, which ArduinoJson rejects.
  const char* json = text.c_str();
  if (text.length() >= 3 && static_cast<uint8_t>(json[0]) == 0xEF && static_cast<uint8_t>(json[1]) == 0xBB &&
      static_cast<uint8_t>(json[2]) == 0xBF) {
    json += 3;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    error = "not valid JSON";
    LOG_ERR("DESK", "%s: %s", PATH, error.c_str());
    return false;
  }
  if (!fromJson(doc.as<JsonVariantConst>(), out, error)) {
    LOG_ERR("DESK", "%s: %s", PATH, error.c_str());
    return false;
  }
  return true;
}
