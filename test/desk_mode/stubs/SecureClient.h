#pragma once

#include <functional>

#include "WiFiClient.h"

namespace freeink {
class SecureClient : public WiFiClient {
 public:
  void setInsecure() {}
  void setCACert(const char*) {}
  void setAbortCallback(const std::function<bool()>&) {}
  bool aborted() { return false; }
  static bool tls13Available() { return false; }
};
}  // namespace freeink
