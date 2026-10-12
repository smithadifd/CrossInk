#pragma once

// Scripted socket for the real SecureHttpClient. A test sets `socketReply` to the bytes the "server" sends and
// `socketBodyStart` to the offset where the body begins. Reads of body bytes cost `socketBodyReadDelayMs` of
// fake time each and return at most `socketReadLimit` bytes, which models a server that trickles its body.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "Arduino.h"

inline std::string socketReply;
inline size_t socketPos = 0;
inline size_t socketBodyStart = 0;
inline uint32_t socketBodyReadDelayMs = 0;
inline size_t socketReadLimit = 2048;

inline void scriptReply(std::string reply) {
  socketReply = std::move(reply);
  socketPos = 0;
  socketBodyStart = socketReply.find("\r\n\r\n") + 4;
  socketBodyReadDelayMs = 0;
  socketReadLimit = 2048;
  fakeClockMs = 0;
}

class Client {
 public:
  virtual ~Client() = default;
  virtual void stop() {}
  virtual int connected() { return socketPos < socketReply.size(); }
  virtual int available() { return static_cast<int>(socketReply.size() - socketPos); }
  virtual int read() {
    return socketPos < socketReply.size() ? static_cast<unsigned char>(socketReply[socketPos++]) : -1;
  }
  virtual int read(uint8_t* dst, size_t n) {
    n = std::min({n, socketReply.size() - socketPos, socketReadLimit});
    if (n && socketPos >= socketBodyStart) fakeClockMs += socketBodyReadDelayMs;
    std::memcpy(dst, socketReply.data() + socketPos, n);
    socketPos += n;
    return static_cast<int>(n);
  }
  virtual size_t write(const uint8_t*, size_t n) { return n; }
};

class WiFiClient : public Client {
 public:
  void setTimeout(uint32_t) {}
  void setConnectionTimeout(uint32_t) {}
  int connect(const char*, uint16_t) { return 1; }
};
