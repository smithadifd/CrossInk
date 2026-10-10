#include "DeskConfig.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <algorithm>
#include <utility>

namespace {
bool isHttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0; }
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
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(PATH, doc)) {
    error = "missing, unreadable or not valid JSON";
    LOG_ERR("DESK", "%s: %s", PATH, error.c_str());
    return false;
  }
  if (!fromJson(doc.as<JsonVariantConst>(), out, error)) {
    LOG_ERR("DESK", "%s: %s", PATH, error.c_str());
    return false;
  }
  return true;
}
