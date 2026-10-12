#pragma once

// Desk mode decisions that do not need the device: timers, image header checks, webhook outcome wording and
// Wi-Fi ownership. Header-only and free of Arduino/ESP types so test/desk_mode runs it on a host.

#include <Bitmap.h>

#include <cstddef>
#include <cstdint>

namespace desk {

// The server renders exactly this bitmap: 1 bit per pixel, portrait.
constexpr int32_t IMAGE_WIDTH = 480;
constexpr int32_t IMAGE_HEIGHT = 800;
constexpr size_t BMP_HEADER_BYTES = 54;  // BITMAPFILEHEADER (14) + BITMAPINFOHEADER (40), before the palette

// Wrap-safe "now has reached at" for 32-bit millisecond counters (valid while the two are under 2^31 ms apart).
inline bool isDue(const uint32_t now, const uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

// Ignores further actions for a short window after one completes. The window is checked against a remaining time
// instead of with isDue() alone: isDue() turns false again once the counter has moved 2^31 ms past the deadline
// (about 25 days), which would silently swallow the first action after a long idle stretch.
class ActionCooldown {
 public:
  void start(const uint32_t now, const uint32_t durationMs) {
    until_ = now + durationMs;
    durationMs_ = durationMs;
    active_ = true;
  }

  // True while an action must be ignored. Call it on every loop pass: it retires an expired window as soon as it
  // is seen, so the window cannot come back later when the counter wraps.
  bool blocks(const uint32_t now) {
    if (!active_) return false;
    const uint32_t remaining = until_ - now;
    if (remaining == 0 || remaining > durationMs_) {
      active_ = false;
      return false;
    }
    return true;
  }

 private:
  uint32_t until_ = 0;
  uint32_t durationMs_ = 0;
  bool active_ = false;
};

// Accepts only the header of a complete 1-bit IMAGE_WIDTH x IMAGE_HEIGHT BMP (either row order) in a file of
// fileSize bytes. The dimensions are compared first, so the size arithmetic below only ever sees 480 x 800 and
// a hostile header cannot overflow it. requiredSize receives the smallest valid file size once the dimensions match.
inline bool bmpHeaderAcceptable(const BmpHeader& header, const uint64_t fileSize, uint64_t& requiredSize) {
  requiredSize = 0;
  const int32_t width = header.infoHeader.biWidth;
  const int32_t height = header.infoHeader.biHeight;
  if (width != IMAGE_WIDTH || (height != IMAGE_HEIGHT && height != -IMAGE_HEIGHT)) return false;
  if (header.fileHeader.bfType != 0x4D42 || header.infoHeader.biSize < 40 || header.infoHeader.biPlanes != 1 ||
      header.infoHeader.biBitCount != 1 || header.infoHeader.biCompression != 0 ||
      header.fileHeader.bfOffBits < BMP_HEADER_BYTES) {
    return false;
  }
  constexpr uint64_t rowBytes = static_cast<uint64_t>((IMAGE_WIDTH + 31) / 32) * 4;
  requiredSize = static_cast<uint64_t>(header.fileHeader.bfOffBits) + rowBytes * static_cast<uint64_t>(IMAGE_HEIGHT);
  return fileSize >= requiredSize;
}

// How an action's webhook request ended, from what the reader can know.
enum class WebhookOutcome : uint8_t {
  SENT,         // 2xx
  HTTP_ERROR,   // the server answered with another status: it received the request and refused it
  NO_WIFI,      // nothing was sent
  BAD_URL,      // nothing was sent
  NO_REPLY,     // no HTTP reply, but the server still accepts connections
  UNCONFIRMED,  // no HTTP reply and the server is not answering now: the request may still have arrived
};

inline WebhookOutcome classifyWebhook(const bool wifiUp, const bool urlValid, const int code,
                                      const bool serverReachable) {
  if (!wifiUp) return WebhookOutcome::NO_WIFI;
  if (!urlValid) return WebhookOutcome::BAD_URL;
  if (code >= 200 && code < 300) return WebhookOutcome::SENT;
  if (code > 0) return WebhookOutcome::HTTP_ERROR;
  return serverReachable ? WebhookOutcome::NO_REPLY : WebhookOutcome::UNCONFIRMED;
}

// The request may have reached the server, so its redraw is worth looking for.
inline bool webhookMayHaveTakenEffect(const WebhookOutcome outcome) {
  return outcome == WebhookOutcome::SENT || outcome == WebhookOutcome::NO_REPLY ||
         outcome == WebhookOutcome::UNCONFIRMED;
}

// Wi-Fi state Desk mode must hand back on exit. Desk mode tears Wi-Fi down only when it was the one to bring it up:
// a station link that was already connected, or an access point another screen is running, belongs to that screen.
// The power-save mode and auto-reconnect it replaced with its own keep-alive tuning are put back as they were.
class RadioLease {
 public:
  struct ExitPlan {
    bool restoreTuning;
    uint8_t sleepMode;
    bool autoReconnect;
    bool teardown;
  };

  void enter(const bool stationConnected, const bool accessPointActive) {
    owns_ = !stationConnected && !accessPointActive;
  }

  // Called just before Desk mode changes the power-save mode and auto-reconnect.
  void rememberTuning(const uint8_t sleepMode, const bool autoReconnect) {
    sleepMode_ = sleepMode;
    autoReconnect_ = autoReconnect;
    tuned_ = true;
  }

  bool ownsRadio() const { return owns_; }

  // radioPresent: the Wi-Fi driver is initialised at exit (mode is not "off").
  ExitPlan onExit(const bool radioPresent) const {
    return ExitPlan{tuned_ && radioPresent, sleepMode_, autoReconnect_, owns_ && radioPresent};
  }

 private:
  bool owns_ = false;
  bool tuned_ = false;
  uint8_t sleepMode_ = 0;
  bool autoReconnect_ = true;
};

}  // namespace desk
