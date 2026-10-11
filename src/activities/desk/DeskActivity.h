#pragma once

#include <cstdint>
#include <string>

#include "DeskConfig.h"
#include "activities/Activity.h"

// Desk mode: keeps a server-rendered 1-bit status screen on the display and turns taps or button
// presses into webhook actions. The activity polls the configured image (optionally gated by a
// small version URL), shows the last good image while offline, and POSTs {"action":"<name>"} to
// the webhook when a zone or button is used. See docs/desk-mode.md.
//
// User-facing text here is deliberately literal rather than tr(STR_*): Desk mode is a private,
// single-language feature. Move it to the I18n tables before proposing it upstream.
class DeskActivity final : public Activity {
 public:
  explicit DeskActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Desk", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Stay awake on USB power (a desk display should not sleep); normal auto-sleep applies on battery.
  bool preventAutoSleep() override;

 private:
  enum class Phase : uint8_t { CONFIG_ERROR, CONNECTING, RUNNING };
  enum class DeskAction : uint8_t { VIDEO_CALL, FOCUS, EVENING, RESTORE, COUNT };

  static constexpr size_t TOAST_CAPACITY = 40;

  // State below is written by loop() under RenderLock and read by render().
  Phase phase = Phase::CONNECTING;
  std::string configError;
  bool online = true;
  bool forceFullRefresh = true;  // half refresh on the next render: first paint/image, every Nth image change, etc.
  char toast[TOAST_CAPACITY] = {0};
  unsigned long toastUntil = 0;

  // Main-task only.
  DeskConfig config;
  GfxRenderer::Orientation savedOrientation = GfxRenderer::Portrait;
  bool wifiTuned = false;
  bool wasWifiConnected = false;
  bool prevAutoReconnect = true;
  bool shouldTearDownWifiOnExit = false;
  bool pollRequested = false;
  bool forceRedownload = false;
  bool haveImageHash = false;
  uint32_t imageHash = 0;
  std::string lastVersion;
  unsigned long nextPollAt = 0;
  int imageChangesSinceFull = 0;
  uint8_t failedPolls = 0;
  uint32_t pollCount = 0;
  bool followUpPending = false;
  unsigned long followUpPollAt = 0;
  bool actionInFlight = false;
  bool cooldownActive = false;
  unsigned long ignoreActionsUntil = 0;

  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void startRunning();

  void pollOnce();
  bool checkForUpdate(bool forced);
  unsigned long nextPollDelayMs() const;
  bool fetchVersion(std::string& out);
  bool downloadToStaging(uint32_t& hash);
  bool promoteStagedImage();
  void recoverStagedImage();
  void setOnline(bool value);

  bool readRequestedAction(DeskAction& action) const;
  bool zoneAt(int x, int y, DeskAction& action) const;
  void sendAction(DeskAction action);
  void setToast(const char* text, unsigned long durationMs);

  void drawImageOrPlaceholder() const;
  void drawConfigError() const;
  void drawOverlays() const;
};
