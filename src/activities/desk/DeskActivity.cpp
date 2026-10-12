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

#include "DeskTransfer.h"
#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "fontIds.h"

namespace {
using desk::BACKUP_PATH;
using desk::IMAGE_DIR;
using desk::IMAGE_PATH;
using desk::STAGED_PATH;

constexpr uint32_t PROBE_TIMEOUT_MS = 1500;                     // reachability probe after a webhook with no reply
constexpr unsigned long TOAST_MS = 2500;                        // how long an action acknowledgement stays up
constexpr unsigned long ACTION_COOLDOWN_MS = 1000;              // ignore further actions after one completes
constexpr unsigned long POLL_AFTER_ACTION_MS = 2000;            // first look for the server's redraw after a webhook
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

bool isDue(const unsigned long now, const unsigned long at) { return desk::isDue(now, at); }

// Accepts only a complete 1-bit 480 x 800 BMP that the renderer can draw. The size check
// catches bodies cut short, which would otherwise draw as a half-black image.
bool validateBmp(const char* path) {
  FsFile file;
  if (!Storage.openFileForRead("DESK", path, file)) return false;

  BmpHeader header{};
  static_assert(offsetof(BmpHeader, colors) == desk::BMP_HEADER_BYTES, "unexpected BMP header layout");
  const bool headerRead = file.read(&header, desk::BMP_HEADER_BYTES) == static_cast<int>(desk::BMP_HEADER_BYTES);
  const size_t fileSize = file.fileSize();
  if (!headerRead) {
    file.close();
    LOG_ERR("DESK", "Image rejected: short header");
    return false;
  }

  uint64_t requiredSize = 0;
  if (!desk::bmpHeaderAcceptable(header, fileSize, requiredSize)) {
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

  // Wi-Fi that is already connected, or an access point another screen is running, is not ours to turn off.
  const bool stationConnected = WiFi.status() == WL_CONNECTED;
  const bool accessPointActive = (WiFi.getMode() & WIFI_MODE_AP) != 0;
  radio.enter(stationConnected, accessPointActive);
  if (stationConnected) {
    startRunning();
    return;
  }
  launchWifiSelection();
}

void DeskActivity::onExit() {
  Activity::onExit();
  // ActivityManager calls onExit() with the render lock held, so no RenderLock here.
  renderer.setOrientation(savedOrientation);

  const desk::RadioLease::ExitPlan plan = radio.onExit(WiFi.getMode() != WIFI_MODE_NULL);
  if (plan.restoreTuning) {
    // Undo the keep-alive tuning even when Wi-Fi was already up and stays up for the next screen.
    WiFi.setSleep(static_cast<wifi_ps_type_t>(plan.sleepMode));
    WiFi.setAutoReconnect(plan.autoReconnect);
  }
  if (plan.teardown) {
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
  radio.rememberTuning(static_cast<uint8_t>(WiFi.getSleep()), WiFi.getAutoReconnect());
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  wasWifiConnected = WiFi.status() == WL_CONNECTED;
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
  // The cooldown is evaluated on every pass so an expired window is retired long before the counter could wrap.
  const bool coolingDown = cooldown.blocks(static_cast<uint32_t>(now));
  DeskAction action;
  if (readRequestedAction(action)) {
    if (coolingDown) {
      LOG_DBG("DESK", "Action ignored during cooldown");
    } else {
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
    imageChangesSinceFull = 0;  // this refresh is the half refresh
    forceRedownload = true;
    pollRequested = true;
    requestUpdate();
  }

  // Wi-Fi came back: poll now rather than waiting out the offline backoff.
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected && !wasWifiConnected) pollRequested = true;
  wasWifiConnected = wifiConnected;

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
  bool urlValid = true;
  if (wifiUp) {
    char body[48];
    snprintf(body, sizeof(body), "{\"action\":\"%s\"}", info.name);
    freeink::SecureHttpClient http;
    code = desk::postWebhook(http, config.webhookUrl, body, urlValid);
  }

  // No HTTP reply is ambiguous: the request may or may not have gone through. The probe only separates a server
  // that still accepts connections ("no reply") from one that is not there ("not confirmed"); it does not prove
  // delivery either way, so neither case is reported as a failure.
  const bool noReply = wifiUp && urlValid && code <= 0 && tcpReachable(config.webhookUrl, PROBE_TIMEOUT_MS);
  const desk::WebhookOutcome outcome = desk::classifyWebhook(wifiUp, urlValid, code, noReply);
  LOG_INF("DESK", "Action %s -> %d (outcome %d)", info.name, code, static_cast<int>(outcome));

  char text[TOAST_CAPACITY];
  switch (outcome) {
    case desk::WebhookOutcome::SENT:
      snprintf(text, sizeof(text), "%s - sent", info.label);
      break;
    case desk::WebhookOutcome::NO_REPLY:
      snprintf(text, sizeof(text), "%s - no reply, check lights", info.label);
      break;
    case desk::WebhookOutcome::UNCONFIRMED:
      snprintf(text, sizeof(text), "%s - not confirmed", info.label);
      break;
    case desk::WebhookOutcome::HTTP_ERROR:
      snprintf(text, sizeof(text), "%s failed (HTTP %d)", info.label, code);
      break;
    case desk::WebhookOutcome::BAD_URL:
      snprintf(text, sizeof(text), "%s failed (bad URL)", info.label);
      break;
    case desk::WebhookOutcome::NO_WIFI:
      snprintf(text, sizeof(text), "%s failed (no Wi-Fi)", info.label);
      break;
  }
  setToast(text, TOAST_MS);

  if (desk::webhookMayHaveTakenEffect(outcome)) {
    // The server usually redraws in response; look for it soon, and again once slower renders have finished.
    const unsigned long now = millis();
    nextPollAt = now + POLL_AFTER_ACTION_MS;
    followUpPending = true;
    followUpPollAt = now + FOLLOW_UP_POLL_AFTER_ACTION_MS;
  }

  actionInFlight = false;
  cooldown.start(static_cast<uint32_t>(millis()), ACTION_COOLDOWN_MS);
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
  return desk::fetchVersionToken(http, config.versionUrl, out);
}

// Streams image_url into status.new and validates it. Takes no RenderLock: nothing here touches status.bmp,
// which the render task may be reading. The whole step, from clearing the old staging file to the last byte, is
// held to IMAGE_BUDGET_MS; each socket operation is further bounded by HTTP_TIMEOUT_MS.
bool DeskActivity::downloadToStaging(uint32_t& hash) {
  const uint32_t deadline = static_cast<uint32_t>(millis()) + desk::IMAGE_BUDGET_MS;
  if (Storage.exists(STAGED_PATH)) Storage.remove(STAGED_PATH);
  FsFile file;
  if (!Storage.openFileForWrite("DESK", STAGED_PATH, file)) {
    LOG_ERR("DESK", "Cannot open %s for writing", STAGED_PATH);
    return false;
  }

  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(desk::HTTP_TIMEOUT_MS);
  if (!http.begin(config.imageUrl)) {
    LOG_ERR("DESK", "Image URL is malformed");
    file.close();
    Storage.remove(STAGED_PATH);
    return false;
  }

  const desk::ImageDownload download = desk::downloadImageBody(http, file, deadline);
  http.end();
  file.close();

  if (!download.ok()) {
    LOG_ERR("DESK", "Image download failed: code=%d written=%u complete=%d writeFailed=%d tooLarge=%d timedOut=%d",
            download.code, static_cast<unsigned>(download.written), download.complete, download.writeFailed,
            download.tooLarge, download.timedOut);
    Storage.remove(STAGED_PATH);
    return false;
  }
  if (!validateBmp(STAGED_PATH)) {
    Storage.remove(STAGED_PATH);
    return false;
  }
  hash = download.hash;
  return true;
}

// Replaces status.bmp with status.new. The render task is locked out only for the renames. The old image is set
// aside rather than deleted, and put back if the new one cannot be moved in, so a failed swap never leaves the
// desk without a picture. If power is lost midway, recoverStagedImage() finishes the job at the next entry.
bool DeskActivity::promoteStagedImage() {
  RenderLock lock;
  return desk::promoteStaged(Storage, IMAGE_PATH, STAGED_PATH, BACKUP_PATH);
}

void DeskActivity::recoverStagedImage() {
  RenderLock lock;
  desk::recoverInterruptedUpdate(Storage, IMAGE_PATH, STAGED_PATH, BACKUP_PATH,
                                 [](const char* path) { return validateBmp(path); });
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
    // The swap may have removed status.bmp before failing, so make the next download repaint even if identical.
    haveImageHash = false;
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
