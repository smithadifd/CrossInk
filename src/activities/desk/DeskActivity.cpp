#include "DeskActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <cctype>
#include <cstdio>

#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr char IMAGE_DIR[] = "/.crosspoint/desk";
constexpr char IMAGE_PATH[] = "/.crosspoint/desk/status.bmp";

constexpr uint32_t HTTP_TIMEOUT_MS = 3000;            // version GET and webhook POST
constexpr size_t MAX_VERSION_BYTES = 128;             // version text is a short token, not a document
constexpr unsigned long TOAST_MS = 2500;              // how long an action acknowledgement stays up
constexpr unsigned long POLL_AFTER_ACTION_MS = 2000;  // give the server time to redraw after a webhook

// The bottom 2x2 grid of touch cells (the server draws matching boxes). Each cell is half the
// screen wide and ZONE_ROW_HEIGHT tall, so the grid occupies the bottom 2 * ZONE_ROW_HEIGHT pixels.
constexpr int ZONE_ROW_HEIGHT = 120;

struct ActionInfo {
  const char* name;   // value of the "action" field in the webhook body
  const char* label;  // shown in the acknowledgement toast
};
constexpr ActionInfo ACTIONS[] = {
    {"video_call", "Video call"},
    {"focus", "Focus"},
    {"evening", "Evening"},
    {"restore", "Restore"},
};

bool isDue(const unsigned long now, const unsigned long at) { return static_cast<long>(now - at) >= 0; }

// Rejects bodies that are not a readable BMP before they can replace the last good image.
bool validateBmp(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("DESK", path, file)) return false;
  Bitmap bitmap(file);
  const BmpReaderError err = bitmap.parseHeaders();
  file.close();
  if (err != BmpReaderError::Ok) {
    LOG_ERR("DESK", "Downloaded image is not a usable BMP: %s", Bitmap::errorToString(err));
    return false;
  }
  return true;
}

// FNV-1a over the file, used to skip a redraw when the server returned an identical image.
bool hashFile(const char* path, uint32_t& hash) {
  FsFile file;
  if (!Storage.openFileForRead("DESK", path, file)) return false;
  uint8_t buf[256];
  uint32_t h = 2166136261u;
  int n;
  while ((n = file.read(buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n; i++) {
      h = (h ^ buf[i]) * 16777619u;
    }
  }
  file.close();
  hash = h;
  return true;
}
}  // namespace

bool DeskActivity::preventAutoSleep() {
#ifdef SIMULATOR
  return gpio.isUsbConnected();
#else
  return gpio.isUsbConnectedCached();
#endif
}

void DeskActivity::onEnter() {
  Activity::onEnter();
  // Free SD-font RAM before network/TLS work, as the other Wi-Fi activities do.
  sdFontSystem.releaseForNetwork(renderer);
  Storage.mkdir(IMAGE_DIR);

  std::string error;
  if (!DeskConfig::load(config, error)) {
    RenderLock lock;
    phase = Phase::CONFIG_ERROR;
    configError = error;
    requestUpdate();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    startRunning();
    return;
  }
  shouldTearDownWifiOnExit = true;
  launchWifiSelection();
}

void DeskActivity::onExit() {
  Activity::onExit();

  if (shouldTearDownWifiOnExit && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
  }
}

void DeskActivity::launchWifiSelection() {
  {
    RenderLock lock;
    phase = Phase::CONNECTING;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void DeskActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    LOG_INF("DESK", "WiFi selection cancelled, leaving Desk mode");
    finish();
    return;
  }
  startRunning();
}

void DeskActivity::startRunning() {
  // Same keep-alive settings as the web server: without them the radio naps between polls and the
  // first request after idle time often fails.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  {
    RenderLock lock;
    phase = Phase::RUNNING;
    forceFullRefresh = true;  // clean repaint after the Wi-Fi screens
  }
  pollRequested = true;  // fetch immediately instead of waiting a full interval
  requestUpdate();
}

void DeskActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  if (phase != Phase::RUNNING) return;

  const unsigned long now = millis();

  if (toast[0] != '\0' && isDue(now, toastUntil)) {
    {
      RenderLock lock;
      toast[0] = '\0';
    }
    requestUpdate();
  }

  DeskAction action;
  if (readRequestedAction(action)) {
    sendAction(action);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Manual refresh: re-fetch regardless of version and repaint with a half refresh to clear ghosting.
    {
      RenderLock lock;
      forceFullRefresh = true;
    }
    forceRedownload = true;
    pollRequested = true;
    requestUpdate();
  }

  if (pollRequested || isDue(now, nextPollAt)) {
    pollOnce();
  }
}

bool DeskActivity::readRequestedAction(DeskAction& action) const {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasPressed(Button::Confirm)) {
    action = DeskAction::VIDEO_CALL;
    return true;
  }
  if (mappedInput.wasPressed(Button::Up)) {
    action = DeskAction::FOCUS;
    return true;
  }
  if (mappedInput.wasPressed(Button::Down)) {
    action = DeskAction::EVENING;
    return true;
  }
  if (mappedInput.wasPressed(Button::Right)) {
    action = DeskAction::RESTORE;
    return true;
  }

  int x = 0;
  int y = 0;
  if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(x, y)) {
    const int gridTop = renderer.getScreenHeight() - 2 * ZONE_ROW_HEIGHT;
    if (y < gridTop) return false;
    const bool right = x >= renderer.getScreenWidth() / 2;
    const bool bottom = y >= gridTop + ZONE_ROW_HEIGHT;
    action = bottom ? (right ? DeskAction::RESTORE : DeskAction::EVENING)
                    : (right ? DeskAction::FOCUS : DeskAction::VIDEO_CALL);
    return true;
  }
  return false;
}

void DeskActivity::sendAction(const DeskAction action) {
  const ActionInfo& info = ACTIONS[static_cast<size_t>(action)];
  char text[TOAST_CAPACITY];

  int code = -1;
  if (WiFi.status() == WL_CONNECTED) {
    char body[48];
    snprintf(body, sizeof(body), "{\"action\":\"%s\"}", info.name);
    freeink::SecureHttpClient http;
    http.setInsecure();  // plain http on the LAN is the expected case; https skips verification like the other clients
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (http.begin(config.webhookUrl)) {
      http.addHeader("Content-Type", "application/json");
      code = http.POST(body);
      http.end();
    } else {
      LOG_ERR("DESK", "Bad webhook URL: %s", config.webhookUrl.c_str());
    }
  }

  const bool ok = code >= 200 && code < 300;
  LOG_INF("DESK", "Action %s -> %d", info.name, code);
  if (ok) {
    snprintf(text, sizeof(text), "%s - sent", info.label);
  } else if (code > 0) {
    snprintf(text, sizeof(text), "%s failed (HTTP %d)", info.label, code);
  } else {
    snprintf(text, sizeof(text), "%s failed (no response)", info.label);
  }
  setToast(text, TOAST_MS);

  if (ok) {
    // The server usually redraws the image in response; look for it soon rather than at the next interval.
    nextPollAt = millis() + POLL_AFTER_ACTION_MS;
  }
}

void DeskActivity::setToast(const char* text, const unsigned long durationMs) {
  {
    RenderLock lock;
    snprintf(toast, sizeof(toast), "%s", text);
    toastUntil = millis() + durationMs;
  }
  requestUpdate();
}

void DeskActivity::setOnline(const bool value) {
  if (online == value) return;
  {
    RenderLock lock;
    online = value;
  }
  requestUpdate();
}

bool DeskActivity::fetchVersion(std::string& out) {
  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(config.versionUrl)) {
    LOG_ERR("DESK", "Bad version URL: %s", config.versionUrl.c_str());
    return false;
  }
  out.clear();
  const int code = http.GET([&out](const uint8_t* data, const size_t len) {
    // Keep only the leading bytes; anything longer is not a version token and would only cost heap.
    const size_t room = MAX_VERSION_BYTES - out.size();
    out.append(reinterpret_cast<const char*>(data), len < room ? len : room);
    return true;
  });
  http.end();
  if (code != 200) {
    LOG_ERR("DESK", "Version GET failed: %d", code);
    return false;
  }
  while (!out.empty() && isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  return true;
}

bool DeskActivity::downloadImage() {
  HttpDownloader::DownloadOptions options;
  // Stage as status.bmp.part and rename only after validateBmp() passes, so a failed or truncated
  // download never replaces the last good image.
  options.stageAsPart = true;
  options.validate = validateBmp;

  // Hold the render lock so the render task cannot be reading status.bmp while it is swapped. Only
  // this task wants to render meanwhile, so nothing else is starved.
  RenderLock lock;
  const auto result = HttpDownloader::downloadToFile(config.imageUrl, IMAGE_PATH, nullptr, nullptr, "", "", options);
  if (result != HttpDownloader::OK) {
    LOG_ERR("DESK", "Image download failed: %d", static_cast<int>(result));
    return false;
  }
  return true;
}

void DeskActivity::pollOnce() {
  const bool forced = forceRedownload;
  pollRequested = false;
  forceRedownload = false;
  nextPollAt = millis() + static_cast<unsigned long>(config.pollS) * 1000UL;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("DESK", "Poll skipped: WiFi down");
    WiFi.reconnect();
    setOnline(false);
    if (forced) requestUpdate();
    return;
  }

  std::string version;
  const bool haveVersionUrl = !config.versionUrl.empty();
  if (haveVersionUrl) {
    if (!fetchVersion(version)) {
      setOnline(false);
      if (forced) requestUpdate();
      return;
    }
    if (!forced && haveImageHash && version == lastVersion) {
      setOnline(true);
      return;
    }
  }

  if (!downloadImage()) {
    setOnline(false);
    if (forced) requestUpdate();
    return;
  }

  uint32_t hash = 0;
  const bool hashed = hashFile(IMAGE_PATH, hash);
  const bool changed = forced || !hashed || !haveImageHash || hash != imageHash;
  imageHash = hash;
  haveImageHash = hashed;
  lastVersion = version;
  setOnline(true);
  if (changed) requestUpdate();
}

void DeskActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (phase == Phase::CONFIG_ERROR) {
    drawConfigError();
  } else {
    drawImageOrPlaceholder();
    drawOverlays();
  }

  // Half refresh on the first paint and every full_refresh_every paints to clear fast-refresh ghosting.
  const bool full = forceFullRefresh || refreshesSinceFull >= config.fullRefreshEvery;
  if (full) {
    forceFullRefresh = false;
    refreshesSinceFull = 0;
  } else {
    refreshesSinceFull++;
  }
  renderer.displayBuffer(full ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}

void DeskActivity::drawImageOrPlaceholder() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  FsFile file;
  if (Storage.exists(IMAGE_PATH) && Storage.openFileForRead("DESK", IMAGE_PATH, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      // The server is expected to send exactly screen-sized art; centre anything smaller.
      const int x = bitmap.getWidth() < pageWidth ? (pageWidth - bitmap.getWidth()) / 2 : 0;
      const int y = bitmap.getHeight() < pageHeight ? (pageHeight - bitmap.getHeight()) / 2 : 0;
      const bool drawn = renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);
      file.close();
      if (drawn) return;
      LOG_ERR("DESK", "Failed to draw %s", IMAGE_PATH);
    } else {
      file.close();
      LOG_ERR("DESK", "Cached image is not a usable BMP");
    }
  }

  const int midY = pageHeight / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, "Desk mode", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, midY + 10,
                            phase == Phase::CONNECTING ? "Connecting..." : "Waiting for image...");
}

void DeskActivity::drawConfigError() const {
  int y = renderer.getScreenHeight() / 2 - 110;
  const int step = renderer.getLineHeight(UI_10_FONT_ID) + 6;
  renderer.drawCenteredText(UI_12_FONT_ID, y, "Desk mode", true, EpdFontFamily::BOLD);
  y += step + 10;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Config not usable:");
  y += step;
  renderer.drawCenteredText(UI_10_FONT_ID, y, DeskConfig::PATH);
  y += step;
  renderer.drawCenteredText(UI_10_FONT_ID, y, configError.c_str());
  y += step * 2;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Expected fields:");
  y += step;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "image_url, webhook_url (required)");
  y += step;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "version_url, poll_s, full_refresh_every");
  y += step * 2;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Press Back to exit");
}

void DeskActivity::drawOverlays() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int pad = 6;

  if (!online) {
    // Small status chip in the top-right corner so the last good image stays visible.
    const char* text = "offline - retrying";
    const int w = renderer.getTextWidth(SMALL_FONT_ID, text) + pad * 2;
    const int h = renderer.getLineHeight(SMALL_FONT_ID) + pad;
    const int x = pageWidth - w;
    renderer.fillRect(x, 0, w, h, false);
    renderer.drawRect(x, 0, w, h, true);
    renderer.drawText(SMALL_FONT_ID, x + pad, pad / 2, text, true);
  }

  if (toast[0] != '\0') {
    // Acknowledgement centred just above the touch grid.
    const int w = renderer.getTextWidth(UI_10_FONT_ID, toast) + pad * 4;
    const int h = renderer.getLineHeight(UI_10_FONT_ID) + pad * 2;
    const int x = (pageWidth - w) / 2;
    const int y = pageHeight - 2 * ZONE_ROW_HEIGHT - h - 12;
    renderer.fillRect(x, y, w, h, false);
    renderer.drawRect(x, y, w, h, 2, true);
    renderer.drawText(UI_10_FONT_ID, x + pad * 2, y + pad, toast, true);
  }
}
