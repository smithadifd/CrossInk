#pragma once

// Desk mode's network and storage steps, written against small interfaces so test/desk_mode runs them on a host
// with the real SecureHttpClient over a scripted socket. Everything here talks to a server that is on the LAN but
// not trusted: every read is bounded in bytes and in absolute time.

#include <Arduino.h>
#include <Logging.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

#include "DeskPolicy.h"

namespace desk {

constexpr char IMAGE_DIR[] = "/.crosspoint/desk";
constexpr char IMAGE_PATH[] = "/.crosspoint/desk/status.bmp";
constexpr char STAGED_PATH[] = "/.crosspoint/desk/status.new";
constexpr char BACKUP_PATH[] = "/.crosspoint/desk/status.old";

constexpr size_t MAX_IMAGE_BYTES = 128 * 1024;  // a valid image is about 48 KB; refuse anything absurd
constexpr size_t MAX_ERROR_BODY_BYTES = 4096;   // an error page is read only this far before the transfer is dropped
constexpr size_t MAX_VERSION_BYTES = 128;       // version text is a short token, not a document
constexpr size_t MAX_WEBHOOK_RESPONSE_BYTES = 4096;  // the webhook reply is ignored; only its status matters

constexpr uint32_t HTTP_TIMEOUT_MS = 3000;    // per socket operation: version GET, image GET, webhook POST
constexpr uint32_t IMAGE_BUDGET_MS = 15000;   // wall-clock cap for one image download, from the first byte of work
constexpr uint32_t VERSION_BUDGET_MS = 6000;  // wall-clock cap for the version request
constexpr uint32_t WEBHOOK_BUDGET_MS = 8000;  // wall-clock cap for one webhook request including its reply

constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

// The HTTP client renews its per-read timeout whenever a byte arrives, so a server that trickles one byte at a time
// can hold a request open indefinitely. The abort callback below is polled on every read, which bounds the total.
inline bool pastDeadline(const uint32_t deadline) { return isDue(static_cast<uint32_t>(millis()), deadline); }

// POSTs body to url. Returns the HTTP status, or -1 without one. The reply body is read and discarded under a byte
// cap and a deadline; the status line alone decides the outcome. urlValid is false when nothing could be sent.
template <class Http>
int postWebhook(Http& http, const std::string& url, const std::string& body, bool& urlValid) {
  http.setInsecure();  // no certificate verification for https, see docs/desk-mode.md
  http.setTimeout(HTTP_TIMEOUT_MS);
  urlValid = http.begin(url);
  if (!urlValid) {
    // The URL carries the webhook secret, so it is never logged.
    LOG_ERR("DESK", "Webhook URL is malformed");
    return -1;
  }
  http.addHeader("Content-Type", "application/json");
  const uint32_t deadline = static_cast<uint32_t>(millis()) + WEBHOOK_BUDGET_MS;
  size_t seen = 0;
  const int code = http.sendRequest(
      "POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&seen](const uint8_t*, const size_t len) {
        seen += len;
        return seen <= MAX_WEBHOOK_RESPONSE_BYTES;
      },
      [deadline]() { return pastDeadline(deadline); });
  http.end();
  return code;
}

// GETs the short version token into out. Only a complete reply counts: a body cut short by the network, a slow
// trickle past the deadline or a non-200 status all fail. A longer-than-token body is not an error: the leading
// MAX_VERSION_BYTES are kept and the transfer stops there.
template <class Http>
bool fetchVersionToken(Http& http, const std::string& url, std::string& out) {
  http.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    LOG_ERR("DESK", "Version URL is malformed");
    return false;
  }
  out.clear();
  const uint32_t deadline = static_cast<uint32_t>(millis()) + VERSION_BUDGET_MS;
  bool capped = false;
  const int code = http.GET(
      [&](const uint8_t* data, const size_t len) {
        const size_t room = MAX_VERSION_BYTES - out.size();
        out.append(reinterpret_cast<const char*>(data), len < room ? len : room);
        if (len >= room) {
          capped = true;
          return false;
        }
        return true;
      },
      [deadline]() { return pastDeadline(deadline); });
  const bool complete = http.responseComplete();
  http.end();
  if (code != 200) {
    LOG_ERR("DESK", "Version GET failed: %d", code);
    return false;
  }
  if (!complete && !capped) {
    LOG_ERR("DESK", "Version reply was cut short");
    return false;
  }
  while (!out.empty() && isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  return true;
}

struct ImageDownload {
  int code = -1;
  size_t written = 0;
  uint32_t hash = FNV_OFFSET;
  bool complete = false;
  bool writeFailed = false;
  bool tooLarge = false;
  bool timedOut = false;

  bool ok() const { return code == 200 && !writeFailed && !tooLarge && complete && written > 0; }
};

// Streams the image reply into file, hashing as it goes. Stops at MAX_IMAGE_BYTES (or MAX_ERROR_BODY_BYTES for a
// non-200 reply) and at the absolute deadline, whatever the server does. A declared length over the cap is refused
// before any of it is stored.
template <class Http, class File>
ImageDownload downloadImageBody(Http& http, File& file, const uint32_t deadline) {
  ImageDownload result;
  size_t ignored = 0;
  result.code = http.GET(
      [&](const uint8_t* data, const size_t len) {
        if (pastDeadline(deadline)) {
          result.timedOut = true;
          return false;
        }
        if (http.getStatus() != 200) {
          ignored += len;  // error page: the status check by the caller reports it
          return ignored <= MAX_ERROR_BODY_BYTES;
        }
        if ((http.hasContentLength() && http.getContentLength() > MAX_IMAGE_BYTES) ||
            result.written + len > MAX_IMAGE_BYTES) {
          result.tooLarge = true;
          return false;
        }
        if (file.write(data, len) != len) {
          result.writeFailed = true;
          return false;
        }
        for (size_t i = 0; i < len; i++) result.hash = (result.hash ^ data[i]) * FNV_PRIME;
        result.written += len;
        return true;
      },
      [deadline]() { return pastDeadline(deadline); });
  result.complete = http.responseComplete() && (!http.hasContentLength() || result.written == http.getContentLength());
  if (!result.complete && pastDeadline(deadline)) result.timedOut = true;
  return result;
}

// Replaces image with staged without ever leaving the image missing: the old file is renamed aside first and put
// back if the new one cannot be moved in. Returns true when staged is now the image.
template <class Store>
bool promoteStaged(Store& store, const char* image, const char* staged, const char* backup) {
  const bool hadImage = store.exists(image);
  if (hadImage) {
    if (store.exists(backup) && !store.remove(backup)) {
      LOG_ERR("DESK", "Cannot clear old backup %s", backup);
      return false;
    }
    if (!store.rename(image, backup)) {
      LOG_ERR("DESK", "Cannot set %s aside", image);
      return false;
    }
  }
  if (!store.rename(staged, image)) {
    LOG_ERR("DESK", "Cannot rename %s to %s", staged, image);
    if (hadImage && !store.rename(backup, image)) {
      LOG_ERR("DESK", "Cannot put %s back; it stays at %s", image, backup);
    }
    return false;
  }
  if (hadImage && !store.remove(backup)) LOG_ERR("DESK", "Cannot remove backup %s", backup);
  return true;
}

// Cleans up after an interrupted update: a missing image is restored from its backup, a complete staged image is
// promoted, an incomplete one is dropped, and a backup that is no longer needed is removed.
template <class Store, class ValidateStaged>
void recoverInterruptedUpdate(Store& store, const char* image, const char* staged, const char* backup,
                              ValidateStaged&& validateStaged) {
  if (!store.exists(image) && store.exists(backup)) {
    LOG_INF("DESK", "Restoring %s from its backup", image);
    store.rename(backup, image);
  }
  if (store.exists(staged)) {
    if (validateStaged(staged)) {
      LOG_INF("DESK", "Promoting staged image left by an interrupted update");
      promoteStaged(store, image, staged, backup);
    } else {
      store.remove(staged);
    }
  }
  if (store.exists(image) && store.exists(backup)) store.remove(backup);
}

}  // namespace desk
