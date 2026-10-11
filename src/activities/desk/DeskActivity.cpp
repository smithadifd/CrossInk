#include "DeskActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "fontIds.h"

namespace {
constexpr char IMAGE_DIR[] = "/.crosspoint/desk";
constexpr char IMAGE_PATH[] = "/.crosspoint/desk/status.bmp";
constexpr char STAGED_PATH[] = "/.crosspoint/desk/status.new";

// The server renders exactly this bitmap: 1 bit per pixel, portrait.
constexpr int32_t IMAGE_WIDTH = 480;
constexpr int32_t IMAGE_HEIGHT = 800;
constexpr size_t MAX_IMAGE_BYTES = 128 * 1024;  // a valid image is about 48 KB; refuse anything absurd

constexpr uint32_t HTTP_TIMEOUT_MS = 3000;            // per socket operation: version GET, image GET, webhook POST
constexpr unsigned long IMAGE_BUDGET_MS = 15000;      // wall-clock cap for one image download
constexpr uint32_t PROBE_TIMEOUT_MS = 1500;           // reachability probe after a webhook with no reply
constexpr size_t MAX_VERSION_BYTES = 128;             // version text is a short token, not a document
constexpr unsigned long TOAST_MS = 2500;              // how long an action acknowledgement stays up
constexpr unsigned long ACTION_COOLDOWN_MS = 1000;    // ignore further actions after one completes
constexpr unsigned long POLL_AFTER_ACTION_MS = 2000;  // first look for the server's redraw after a webhook
constexpr unsigned long FOLLOW_UP_POLL_AFTER_ACTION_MS = 6000;  // second look, for slower server renders
constexpr unsigned long MAX_BACKOFF_S = 300;                    // offline backoff grows to this (never below poll_s)
constexpr uint8_t MAX_BACKOFF_DOUBLINGS = 8;
constexpr uint32_t HEAP_LOG_EVERY_POLLS = 10;

// Touch zones: the bottom 2x2 grid of cells. Each cell is half the screen wide and ZONE_ROW_HEIGHT tall. A
// rectangle is its cell inset by ZONE_INSET, matching the boxes the server draws. The bottom row is also
// cut off where the bottom-edge gesture band starts (the same 6% MappedInputManager reserves), so a tap
// never lands in that band.
constexpr int ZONE_ROW_HEIGHT = 120;
constexpr int ZONE_INSET = 8;
constexpr int BOTTOM_EDGE_BAND_PERCENT = 6;

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

constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

bool isDue(const unsigned long now, const unsigned long at) { return static_cast<long>(now - at) >= 0; }

// Accepts only a complete 1-bit IMAGE_WIDTH x IMAGE_HEIGHT BMP that the renderer can draw. The size check
// catches bodies cut short, which would otherwise draw as a half-black image.
bool validateBmp(const char* path) {
  FsFile file;
  if (!Storage.openFileForRead("DESK", path, file)) return false;

  BmpHeader header{};
  constexpr size_t headerBytes = 54;  // BITMAPFILEHEADER (14) + BITMAPINFOHEADER (40), before the palette
  static_assert(offsetof(BmpHeader, colors) == headerBytes, "unexpected BMP header layout");
  const bool headerRead = file.read(&header, headerBytes) == static_cast<int>(headerBytes);
  const size_t fileSize = file.fileSize();
  if (!headerRead) {
    file.close();
    LOG_ERR("DESK", "Image rejected: short header");
    return false;
  }

  const int32_t height = header.infoHeader.biHeight < 0 ? -header.infoHeader.biHeight : header.infoHeader.biHeight;
  const uint64_t rowBytes = static_cast<uint64_t>((header.infoHeader.biWidth + 31) / 32) * 4;
  const uint64_t requiredSize =
      static_cast<uint64_t>(header.fileHeader.bfOffBits) + rowBytes * static_cast<uint64_t>(height);
  if (header.fileHeader.bfType != 0x4D42 || header.infoHeader.biSize < 40 || header.infoHeader.biPlanes != 1 ||
      header.infoHeader.biBitCount != 1 || header.infoHeader.biCompression != 0 ||
      header.infoHeader.biWidth != IMAGE_WIDTH || height != IMAGE_HEIGHT || header.fileHeader.bfOffBits < headerBytes ||
      fileSize < requiredSize) {
    file.close();
    LOG_ERR("DESK", "Image rejected: bpp=%u w=%ld h=%ld size=%u need=%u", header.infoHeader.biBitCount,
            static_cast<long>(header.infoHeader.biWidth), static_cast<long>(header.infoHeader.biHeight),
            static_cast<unsigned>(fileSize), static_cast<unsigned>(requiredSize));
    return false;
  }

  Bitmap bitmap(file);
  const BmpReaderError err = bitmap.parseHeaders();
  file.close();
  if (err != BmpReaderError::Ok) {
    LOG_ERR("DESK", "Image rejected by renderer: %s", Bitmap::errorToString(err));
    return false;
  }
  return true;
}

// True if a TCP connection to the URL's host:port can be opened. Used only after a webhook got no HTTP reply,
// to tell "the server took the request but did not answer" from "the server is not there at all".
bool tcpReachable(const std::string& url, const uint32_t timeoutMs) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;
  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  const std::string hostPort =
      pathStart == std::string::npos ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
  const size_t portSep = hostPort.rfind(':');
  const std::string host = portSep == std::string::npos ? hostPort : hostPort.substr(0, portSep);
  const bool https = strncasecmp(url.c_str(), "https", 5) == 0;
  const uint16_t port =
      portSep == std::string::npos ? (https ? 443 : 80) : static_cast<uint16_t>(atoi(hostPort.c_str() + portSep + 1));
  if (host.empty()) return false;

  WiFiClient client;
  client.setConnectionTimeout(timeoutMs);
  const bool connected = client.connect(host.c_str(), port);
  client.stop();
  return connected;
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
  {
    // The image and touch zones are laid out for portrait whatever orientation the reader last used.
    RenderLock lock;
    savedOrientation = renderer.getOrientation();
    renderer.setOrientation(GfxRenderer::Portrait);
  }
  // Free SD-font RAM before network/TLS work, as the other Wi-Fi activities do.
  sdFontSystem.releaseForNetwork(renderer);
  Storage.mkdir(IMAGE_DIR);
  recoverStagedImage();

  std::string error;
  if (!DeskConfig::load(config, error)) {
    {
      RenderLock lock;
      phase = Phase::CONFIG_ERROR;
      configError = error;
    }
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
  // ActivityManager calls onExit() with the render lock held, so no RenderLock here.
  renderer.setOrientation(savedOrientation);

  if (wifiTuned && WiFi.getMode() != WIFI_MODE_NULL) {
    // Undo the keep-alive tuning even when Wi-Fi was already up and stays up for the next screen.
    WiFi.setSleep(true);
    WiFi.setAutoReconnect(prevAutoReconnect);
  }
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
  // first request after idle time often fails. onExit() restores the previous values.
  prevAutoReconnect = WiFi.getAutoReconnect();
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  wifiTuned = true;
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

  // Always read the input so a press during the cooldown is consumed rather than replayed afterwards.
  DeskAction action;
  if (readRequestedAction(action)) {
    if (cooldownActive && !isDue(now, ignoreActionsUntil)) {
      LOG_DBG("DESK", "Action ignored during cooldown");
    } else {
      cooldownActive = false;
      sendAction(action);
      return;
    }
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

  if (followUpPending && isDue(now, followUpPollAt)) {
    followUpPending = false;
    pollRequested = true;
  }

  if (pollRequested || isDue(now, nextPollAt)) {
    pollOnce();
  }
}

bool DeskActivity::zoneAt(const int x, const int y, DeskAction& action) const {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int gridTop = height - 2 * ZONE_ROW_HEIGHT;
  const int bottomLimit = height - height * BOTTOM_EDGE_BAND_PERCENT / 100;
  if (y < gridTop) return false;

  const int col = x >= width / 2 ? 1 : 0;
  const int row = y >= gridTop + ZONE_ROW_HEIGHT ? 1 : 0;
  const int left = col * (width / 2) + ZONE_INSET;
  const int right = (col + 1) * (width / 2) - ZONE_INSET;
  const int top = gridTop + row * ZONE_ROW_HEIGHT + ZONE_INSET;
  const int bottom = std::min(gridTop + (row + 1) * ZONE_ROW_HEIGHT - ZONE_INSET, bottomLimit);
  if (x < left || x >= right || y < top || y >= bottom) return false;

  action = row == 0 ? (col == 0 ? DeskAction::VIDEO_CALL : DeskAction::FOCUS)
                    : (col == 0 ? DeskAction::EVENING : DeskAction::RESTORE);
  return true;
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
    return zoneAt(x, y, action);
  }
  return false;
}

void DeskActivity::sendAction(const DeskAction action) {
  if (actionInFlight) return;
  actionInFlight = true;

  const ActionInfo& info = ACTIONS[static_cast<size_t>(action)];
  const bool wifiUp = WiFi.status() == WL_CONNECTED;
  int code = -1;
  if (wifiUp) {
    char body[48];
    snprintf(body, sizeof(body), "{\"action\":\"%s\"}", info.name);
    freeink::SecureHttpClient http;
    http.setInsecure();  // no certificate verification for https, see docs/desk-mode.md
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (http.begin(config.webhookUrl)) {
      http.addHeader("Content-Type", "application/json");
      code = http.POST(body);
      http.end();
    } else {
      LOG_ERR("DESK", "Bad webhook URL: %s", config.webhookUrl.c_str());
    }
  }

  // A transport failure after the request may already have gone out is not a failure the user should retry:
  // if the server still accepts connections, report "sent (no reply)" so they do not press again.
  const bool ok = code >= 200 && code < 300;
  const bool noReply = !ok && code <= 0 && wifiUp && tcpReachable(config.webhookUrl, PROBE_TIMEOUT_MS);
  LOG_INF("DESK", "Action %s -> %d%s", info.name, code, noReply ? " (no reply, server reachable)" : "");

  char text[TOAST_CAPACITY];
  if (ok) {
    snprintf(text, sizeof(text), "%s - sent", info.label);
  } else if (noReply) {
    snprintf(text, sizeof(text), "%s - sent (no reply)", info.label);
  } else if (code > 0) {
    snprintf(text, sizeof(text), "%s failed (HTTP %d)", info.label, code);
  } else if (!wifiUp) {
    snprintf(text, sizeof(text), "%s failed (no Wi-Fi)", info.label);
  } else {
    snprintf(text, sizeof(text), "%s failed (unreachable)", info.label);
  }
  setToast(text, TOAST_MS);

  if (ok || noReply) {
    // The server usually redraws in response; look for it soon, and again once slower renders have finished.
    const unsigned long now = millis();
    nextPollAt = now + POLL_AFTER_ACTION_MS;
    followUpPending = true;
    followUpPollAt = now + FOLLOW_UP_POLL_AFTER_ACTION_MS;
  }

  actionInFlight = false;
  cooldownActive = true;
  ignoreActionsUntil = millis() + ACTION_COOLDOWN_MS;
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

// Streams image_url into status.new and validates it. Takes no RenderLock: nothing here touches status.bmp,
// which the render task may be reading. Bounded by HTTP_TIMEOUT_MS per socket operation and IMAGE_BUDGET_MS overall.
bool DeskActivity::downloadToStaging(uint32_t& hash) {
  if (Storage.exists(STAGED_PATH)) Storage.remove(STAGED_PATH);
  FsFile file;
  if (!Storage.openFileForWrite("DESK", STAGED_PATH, file)) {
    LOG_ERR("DESK", "Cannot open %s for writing", STAGED_PATH);
    return false;
  }

  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(config.imageUrl)) {
    LOG_ERR("DESK", "Bad image URL: %s", config.imageUrl.c_str());
    file.close();
    Storage.remove(STAGED_PATH);
    return false;
  }

  const unsigned long deadline = millis() + IMAGE_BUDGET_MS;
  size_t written = 0;
  uint32_t running = FNV_OFFSET;
  bool writeFailed = false;
  bool tooLarge = false;
  const int code = http.GET(
      [&](const uint8_t* data, const size_t len) {
        if (http.getStatus() != 200) return true;  // error page: counted by the status check below
        if (written + len > MAX_IMAGE_BYTES) {
          tooLarge = true;
          return false;
        }
        if (file.write(data, len) != len) {
          writeFailed = true;
          return false;
        }
        for (size_t i = 0; i < len; i++) running = (running ^ data[i]) * FNV_PRIME;
        written += len;
        return !isDue(millis(), deadline);
      },
      [deadline]() { return isDue(millis(), deadline); });

  const bool complete = http.responseComplete() && (!http.hasContentLength() || written == http.getContentLength());
  http.end();
  file.close();

  if (code != 200 || writeFailed || tooLarge || !complete || written == 0) {
    LOG_ERR("DESK", "Image download failed: code=%d written=%u complete=%d writeFailed=%d tooLarge=%d", code,
            static_cast<unsigned>(written), complete, writeFailed, tooLarge);
    Storage.remove(STAGED_PATH);
    return false;
  }
  if (!validateBmp(STAGED_PATH)) {
    Storage.remove(STAGED_PATH);
    return false;
  }
  hash = running;
  return true;
}

// Replaces status.bmp with status.new. The render task is locked out only for the remove and rename. If power
// is lost between them, status.new still holds a complete validated image and recoverStagedImage() promotes it.
bool DeskActivity::promoteStagedImage() {
  RenderLock lock;
  if (Storage.exists(IMAGE_PATH) && !Storage.remove(IMAGE_PATH)) {
    LOG_ERR("DESK", "Cannot remove old %s", IMAGE_PATH);
    return false;
  }
  if (!Storage.rename(STAGED_PATH, IMAGE_PATH)) {
    LOG_ERR("DESK", "Cannot rename %s to %s", STAGED_PATH, IMAGE_PATH);
    return false;
  }
  return true;
}

void DeskActivity::recoverStagedImage() {
  if (!Storage.exists(STAGED_PATH)) return;
  if (validateBmp(STAGED_PATH)) {
    LOG_INF("DESK", "Promoting staged image left by an interrupted update");
    promoteStagedImage();
  } else {
    Storage.remove(STAGED_PATH);
  }
}

unsigned long DeskActivity::nextPollDelayMs() const {
  const unsigned long base = static_cast<unsigned long>(config.pollS);
  const unsigned long cap = std::max(base, MAX_BACKOFF_S);
  unsigned long seconds = base;
  for (uint8_t i = 0; i < failedPolls && seconds < cap; i++) seconds *= 2;
  return std::min(seconds, cap) * 1000UL;
}

// Returns true when the server was reachable (whether or not the image changed).
bool DeskActivity::checkForUpdate(const bool forced) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("DESK", "Poll skipped: WiFi down");
    WiFi.reconnect();
    setOnline(false);
    return false;
  }

  std::string version;
  if (!config.versionUrl.empty()) {
    if (!fetchVersion(version)) {
      setOnline(false);
      return false;
    }
    // An empty version carries no information, so it never counts as "unchanged".
    if (!forced && !version.empty() && haveImageHash && version == lastVersion) {
      setOnline(true);
      return true;
    }
  }

  uint32_t hash = 0;
  if (!downloadToStaging(hash)) {
    setOnline(false);
    return false;
  }

  const bool changed = forced || !haveImageHash || hash != imageHash;
  if (!changed) {
    lastVersion = version;
    Storage.remove(STAGED_PATH);
    setOnline(true);
    return true;
  }
  if (!promoteStagedImage()) {
    setOnline(false);
    return false;
  }

  // Image changes repaint with a fast refresh (a server that redraws every minute must not flash the panel).
  // The first image after entering Desk mode and every full_refresh_every-th changed image get a half refresh.
  const bool firstImage = !haveImageHash;
  const bool imageDiffers = firstImage || hash != imageHash;
  if (imageDiffers) imageChangesSinceFull++;
  const bool halfRefresh = firstImage || imageChangesSinceFull >= config.fullRefreshEvery;
  if (halfRefresh) imageChangesSinceFull = 0;

  imageHash = hash;
  haveImageHash = true;
  lastVersion = version;  // only after the swap, so a failed swap is retried on the next poll
  {
    RenderLock lock;
    online = true;
    if (halfRefresh) forceFullRefresh = true;
  }
  requestUpdate();
  return true;
}

void DeskActivity::pollOnce() {
  const bool forced = forceRedownload;
  pollRequested = false;
  forceRedownload = false;

  if (++pollCount % HEAP_LOG_EVERY_POLLS == 0) {
    LOG_INF("DESK", "Poll %lu: heap free=%u maxAlloc=%u failedPolls=%u", static_cast<unsigned long>(pollCount),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
            static_cast<unsigned>(failedPolls));
  }

  const bool reachable = checkForUpdate(forced);
  failedPolls = reachable ? 0 : static_cast<uint8_t>(std::min<int>(failedPolls + 1, MAX_BACKOFF_DOUBLINGS));
  // Scheduled after the poll finishes, so a slow or failing poll never makes the next one start immediately.
  nextPollAt = millis() + nextPollDelayMs();
}

void DeskActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (phase == Phase::CONFIG_ERROR) {
    drawConfigError();
  } else {
    drawImageOrPlaceholder();
    drawOverlays();
  }

  // Half refresh clears fast-refresh ghosting. forceFullRefresh is set for the first paint, the first image,
  // every full_refresh_every-th image change, a manual refresh, and the return from the Wi-Fi screens.
  const bool full = forceFullRefresh;
  forceFullRefresh = false;
  renderer.displayBuffer(full ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}

void DeskActivity::drawImageOrPlaceholder() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  FsFile file;
  if (Storage.exists(IMAGE_PATH) && Storage.openFileForRead("DESK", IMAGE_PATH, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      // The server sends exactly screen-sized art; centre anything smaller.
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
