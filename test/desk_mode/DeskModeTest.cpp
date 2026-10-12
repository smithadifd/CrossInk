// Host tests for Desk mode's network, image and Wi-Fi policy. They compile the production headers
// (DeskPolicy.h, DeskTransfer.h) and the real SecureHttpClient against a scripted socket and a fake clock; see
// run.sh. Usage: DeskModeTest [test-name]. With a name, only that test runs.

#include <SecureHttpClient.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "DeskPolicy.h"
#include "DeskTransfer.h"

namespace {

int failures = 0;
const char* currentTest = "";

#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::printf("FAIL %s: %s:%d: %s\n", currentTest, __FILE__, __LINE__, #cond); \
      failures++;                                                                  \
    }                                                                              \
  } while (0)

#define CHECK_MSG(cond, ...)                                                         \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::printf("FAIL %s: %s:%d: %s -- ", currentTest, __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                                      \
      std::printf("\n");                                                             \
      failures++;                                                                    \
    }                                                                                \
  } while (0)

std::string ok200(const std::string& body, const std::string& extraHeaders = "") {
  return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extraHeaders + "\r\n" + body;
}

// A reply that promises `declared` bytes but carries `actual`.
std::string replyWithLength(const int status, const size_t declared, const std::string& body) {
  return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " + std::to_string(declared) + "\r\n\r\n" + body;
}

constexpr size_t SOCKET_CHUNK = 2048;  // the HTTP client's read size

// ---- webhook -------------------------------------------------------------------------------------------------

void webhook_reply_is_read_under_a_byte_cap() {
  scriptReply(ok200(std::string(1048576, 'a')));
  freeink::SecureHttpClient http;
  bool urlValid = false;
  const int code = desk::postWebhook(http, "http://ha/api/webhook/test", "{\"action\":\"focus\"}", urlValid);
  const size_t consumed = socketPos - socketBodyStart;
  CHECK(urlValid);
  CHECK(code == 200);
  CHECK_MSG(http.getSize() == 0, "retained %d bytes of the reply", http.getSize());
  CHECK_MSG(consumed <= desk::MAX_WEBHOOK_RESPONSE_BYTES + SOCKET_CHUNK, "read %zu body bytes", consumed);
}

void webhook_trickled_reply_stops_at_the_deadline() {
  scriptReply(ok200(std::string(100000, 'a')));
  socketBodyReadDelayMs = 2000;
  socketReadLimit = 1;
  freeink::SecureHttpClient http;
  bool urlValid = false;
  const int code = desk::postWebhook(http, "http://ha/api/webhook/test", "{}", urlValid);
  CHECK(code == 200);  // the status line arrived; only the useless body was cut off
  CHECK_MSG(fakeClockMs <= desk::WEBHOOK_BUDGET_MS + 2 * 2000, "request held open for %u ms", fakeClockMs);
}

void webhook_malformed_url_is_not_logged() {
  capturedLog.clear();
  scriptReply(ok200(""));
  freeink::SecureHttpClient http;
  bool urlValid = true;
  const int code = desk::postWebhook(http, "not-a-url/api/webhook/SECRET-TOKEN-123", "{}", urlValid);
  CHECK(!urlValid);
  CHECK(code == -1);
  CHECK_MSG(capturedLog.find("SECRET-TOKEN-123") == std::string::npos, "log was: %s", capturedLog.c_str());
  CHECK_MSG(capturedLog.find("api/webhook") == std::string::npos, "log was: %s", capturedLog.c_str());
}

void webhook_outcomes_never_call_an_unconfirmed_send_a_failure() {
  using desk::classifyWebhook;
  using desk::WebhookOutcome;
  CHECK(classifyWebhook(true, true, 200, false) == WebhookOutcome::SENT);
  CHECK(classifyWebhook(true, true, 204, false) == WebhookOutcome::SENT);
  CHECK(classifyWebhook(true, true, 500, false) == WebhookOutcome::HTTP_ERROR);
  CHECK(classifyWebhook(true, true, 302, true) == WebhookOutcome::HTTP_ERROR);
  CHECK(classifyWebhook(false, true, -1, false) == WebhookOutcome::NO_WIFI);
  CHECK(classifyWebhook(true, false, -1, false) == WebhookOutcome::BAD_URL);
  CHECK(classifyWebhook(true, true, -1, true) == WebhookOutcome::NO_REPLY);
  // The request may have been written before the connection died, so an unreachable server is "unconfirmed".
  CHECK(classifyWebhook(true, true, -1, false) == WebhookOutcome::UNCONFIRMED);
  CHECK(classifyWebhook(true, true, 0, false) == WebhookOutcome::UNCONFIRMED);
  CHECK(desk::webhookMayHaveTakenEffect(WebhookOutcome::UNCONFIRMED));
  CHECK(desk::webhookMayHaveTakenEffect(WebhookOutcome::NO_REPLY));
  CHECK(desk::webhookMayHaveTakenEffect(WebhookOutcome::SENT));
  CHECK(!desk::webhookMayHaveTakenEffect(WebhookOutcome::BAD_URL));
  CHECK(!desk::webhookMayHaveTakenEffect(WebhookOutcome::NO_WIFI));
  CHECK(!desk::webhookMayHaveTakenEffect(WebhookOutcome::HTTP_ERROR));
}

// ---- version -------------------------------------------------------------------------------------------------

void version_trickle_stops_at_the_deadline() {
  scriptReply(ok200(std::string(130, 'v')));
  socketBodyReadDelayMs = 2000;
  socketReadLimit = 1;
  freeink::SecureHttpClient http;
  std::string out;
  const bool ok = desk::fetchVersionToken(http, "http://desk/version", out);
  CHECK(!ok);
  CHECK_MSG(fakeClockMs <= desk::VERSION_BUDGET_MS + 2 * 2000, "request held open for %u ms", fakeClockMs);
}

void version_stops_reading_at_the_token_cap() {
  scriptReply(ok200(std::string(100000, 'v')));
  freeink::SecureHttpClient http;
  std::string out;
  const bool ok = desk::fetchVersionToken(http, "http://desk/version", out);
  CHECK(ok);
  CHECK_MSG(out.size() == desk::MAX_VERSION_BYTES, "kept %zu bytes", out.size());
  const size_t consumed = socketPos - socketBodyStart;
  CHECK_MSG(consumed <= SOCKET_CHUNK, "read %zu body bytes", consumed);
}

void version_cut_short_is_rejected() {
  scriptReply(replyWithLength(200, 20, "v1"));  // promises 20 bytes, delivers 2, then closes
  freeink::SecureHttpClient http;
  std::string out;
  CHECK(!desk::fetchVersionToken(http, "http://desk/version", out));
}

void version_complete_reply_is_accepted_and_trimmed() {
  scriptReply(ok200("2026-10-11T07:00:00Z\n"));
  freeink::SecureHttpClient http;
  std::string out;
  CHECK(desk::fetchVersionToken(http, "http://desk/version", out));
  CHECK(out == "2026-10-11T07:00:00Z");
}

void version_error_status_is_rejected() {
  scriptReply("HTTP/1.1 503 X\r\nContent-Length: 2\r\n\r\nno");
  freeink::SecureHttpClient http;
  std::string out;
  CHECK(!desk::fetchVersionToken(http, "http://desk/version", out));
}

// ---- image download ------------------------------------------------------------------------------------------

struct FakeFile {
  size_t total = 0;
  size_t limit = SIZE_MAX;  // writes past this many bytes fail
  size_t write(const uint8_t*, size_t len) {
    if (total + len > limit) return 0;
    total += len;
    return len;
  }
};

desk::ImageDownload runDownload(FakeFile& file, const uint32_t budgetMs = desk::IMAGE_BUDGET_MS) {
  freeink::SecureHttpClient http;
  CHECK(http.begin("http://desk/status.bmp"));
  const uint32_t deadline = static_cast<uint32_t>(millis()) + budgetMs;
  return desk::downloadImageBody(http, file, deadline);
}

void image_download_succeeds_and_hashes() {
  std::string body(48062, 0);
  for (size_t i = 0; i < body.size(); i++) body[i] = static_cast<char>(i * 7);
  scriptReply(ok200(body));
  FakeFile file;
  const desk::ImageDownload result = runDownload(file);
  uint32_t expected = desk::FNV_OFFSET;
  for (const char c : body) expected = (expected ^ static_cast<uint8_t>(c)) * desk::FNV_PRIME;
  CHECK(result.ok());
  CHECK(result.written == body.size());
  CHECK(file.total == body.size());
  CHECK(result.hash == expected);
}

void image_error_page_is_read_under_a_byte_cap() {
  scriptReply(replyWithLength(404, 1048576, std::string(1048576, 'e')));
  FakeFile file;
  const desk::ImageDownload result = runDownload(file);
  CHECK(!result.ok());
  CHECK(result.code == 404);
  CHECK(file.total == 0);
  const size_t consumed = socketPos - socketBodyStart;
  CHECK_MSG(consumed <= desk::MAX_ERROR_BODY_BYTES + SOCKET_CHUNK, "read %zu body bytes of an error page", consumed);
}

void image_declared_length_over_the_cap_is_refused_unread() {
  scriptReply(replyWithLength(200, 1048576, std::string(1048576, 'b')));
  FakeFile file;
  const desk::ImageDownload result = runDownload(file);
  CHECK(!result.ok());
  CHECK(result.tooLarge);
  CHECK_MSG(file.total == 0, "stored %zu bytes of an image that declared itself too large", file.total);
  const size_t consumed = socketPos - socketBodyStart;
  CHECK_MSG(consumed <= SOCKET_CHUNK, "read %zu body bytes", consumed);
}

void image_without_a_length_stops_at_the_byte_cap() {
  std::string reply = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + std::string(400000, 'c');
  scriptReply(reply);
  FakeFile file;
  const desk::ImageDownload result = runDownload(file);
  CHECK(!result.ok());
  CHECK(result.tooLarge);
  CHECK_MSG(file.total <= desk::MAX_IMAGE_BYTES, "stored %zu bytes", file.total);
}

void image_trickle_stops_at_the_deadline() {
  scriptReply(ok200(std::string(50000, 'd')));
  socketBodyReadDelayMs = 2000;
  socketReadLimit = 1;
  FakeFile file;
  const desk::ImageDownload result = runDownload(file);
  CHECK(!result.ok());
  CHECK_MSG(fakeClockMs <= desk::IMAGE_BUDGET_MS + 2 * 2000, "download held open for %u ms", fakeClockMs);
}

void image_deadline_counts_time_spent_before_the_transfer() {
  // The caller starts the budget before it clears staging and opens the file; a budget already spent stops
  // the very first chunk, even from a server that answers instantly.
  scriptReply(ok200(std::string(48062, 'f')));
  FakeFile file;
  freeink::SecureHttpClient http;
  CHECK(http.begin("http://desk/status.bmp"));
  const uint32_t deadline = static_cast<uint32_t>(millis());  // already due
  const desk::ImageDownload result = desk::downloadImageBody(http, file, deadline);
  CHECK(!result.ok());
  CHECK(result.timedOut);
  CHECK(file.total == 0);
}

void image_write_failure_fails_the_download() {
  scriptReply(ok200(std::string(48062, 'w')));
  FakeFile file;
  file.limit = 1000;
  const desk::ImageDownload result = runDownload(file);
  CHECK(!result.ok());
  CHECK(result.writeFailed);
}

void image_cut_short_by_the_network_fails() {
  scriptReply(replyWithLength(200, 48062, std::string(1000, 'x')));
  FakeFile file;
  const desk::ImageDownload result = runDownload(file);
  CHECK(!result.ok());
  CHECK(!result.complete);
}

// ---- BMP header ----------------------------------------------------------------------------------------------

BmpHeader makeHeader(const int32_t width = 480, const int32_t height = 800, const uint16_t bpp = 1,
                     const uint32_t offset = 62) {
  BmpHeader header{};
  header.fileHeader.bfType = 0x4D42;
  header.fileHeader.bfOffBits = offset;
  header.infoHeader.biSize = 40;
  header.infoHeader.biWidth = width;
  header.infoHeader.biHeight = height;
  header.infoHeader.biPlanes = 1;
  header.infoHeader.biBitCount = bpp;
  return header;
}

constexpr uint64_t VALID_SIZE = 62 + 60 * 800;

void bmp_header_accepts_the_desk_image() {
  uint64_t need = 0;
  CHECK(desk::bmpHeaderAcceptable(makeHeader(), VALID_SIZE, need));
  CHECK(need == VALID_SIZE);
  CHECK(desk::bmpHeaderAcceptable(makeHeader(480, -800), VALID_SIZE, need));  // top-down
  CHECK(!desk::bmpHeaderAcceptable(makeHeader(), VALID_SIZE - 1, need));      // one byte short
  CHECK(!desk::bmpHeaderAcceptable(makeHeader(480, 800, 8), VALID_SIZE, need));
  CHECK(!desk::bmpHeaderAcceptable(makeHeader(480, 800, 1, 53), VALID_SIZE, need));
  CHECK(!desk::bmpHeaderAcceptable(makeHeader(481, 800), VALID_SIZE, need));
  CHECK(!desk::bmpHeaderAcceptable(makeHeader(480, 799), VALID_SIZE, need));
  CHECK(!desk::bmpHeaderAcceptable(makeHeader(800, 480), VALID_SIZE, need));
  BmpHeader wrongMagic = makeHeader();
  wrongMagic.fileHeader.bfType = 0x4D43;
  CHECK(!desk::bmpHeaderAcceptable(wrongMagic, VALID_SIZE, need));
}

// Built with UBSan and no recovery, so the old signed overflow in these two cases aborts the run.
void bmp_header_extreme_dimensions_are_rejected_without_overflow() {
  uint64_t need = 1;
  CHECK(!desk::bmpHeaderAcceptable(makeHeader(INT_MAX, 800), UINT64_MAX, need));
  CHECK(need == 0);  // the dimensions were refused before any size arithmetic
  need = 1;
  CHECK(!desk::bmpHeaderAcceptable(makeHeader(480, INT_MIN), UINT64_MAX, need));
  CHECK(need == 0);
  CHECK(!desk::bmpHeaderAcceptable(makeHeader(INT_MIN, INT_MIN), UINT64_MAX, need));
  BmpHeader hugeOffset = makeHeader();
  hugeOffset.fileHeader.bfOffBits = UINT32_MAX;
  CHECK(!desk::bmpHeaderAcceptable(hugeOffset, VALID_SIZE, need));
  CHECK(need == UINT64_C(4294967295) + 60 * 800);
}

// ---- image swap ----------------------------------------------------------------------------------------------

struct FakeStore {
  std::map<std::string, std::string> files;
  std::string failRenameFrom;  // renames of this path fail
  bool exists(const char* path) { return files.count(path) != 0; }
  bool remove(const char* path) { return files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (!files.count(from) || files.count(to) || failRenameFrom == from) return false;  // SdFat will not overwrite
    files[to] = files[from];
    files.erase(from);
    return true;
  }
};

void swap_failure_keeps_the_old_image() {
  FakeStore store;
  store.files = {{"img", "old"}, {"new", "fresh"}};
  store.failRenameFrom = "new";
  CHECK(!desk::promoteStaged(store, "img", "new", "old"));
  CHECK_MSG(store.exists("img") && store.files["img"] == "old", "image missing or changed after a failed swap");
}

void swap_success_replaces_the_image_and_leaves_no_backup() {
  FakeStore store;
  store.files = {{"img", "old"}, {"new", "fresh"}};
  CHECK(desk::promoteStaged(store, "img", "new", "old"));
  CHECK(store.files["img"] == "fresh");
  CHECK(!store.exists("new"));
  CHECK(!store.exists("old"));
}

void swap_with_no_previous_image_just_installs() {
  FakeStore store;
  store.files = {{"new", "fresh"}};
  CHECK(desk::promoteStaged(store, "img", "new", "old"));
  CHECK(store.files["img"] == "fresh");
}

void swap_replaces_a_stale_backup() {
  FakeStore store;
  store.files = {{"img", "old"}, {"new", "fresh"}, {"old", "stale"}};
  CHECK(desk::promoteStaged(store, "img", "new", "old"));
  CHECK(store.files["img"] == "fresh");
  CHECK(!store.exists("old"));
}

void recovery_after_power_loss_between_the_renames() {
  const auto valid = [](const char* path) { return std::string(path) == "new"; };
  FakeStore store;
  store.files = {{"old", "previous"}, {"new", "fresh"}};  // image set aside, staged not yet moved in
  desk::recoverInterruptedUpdate(store, "img", "new", "old", valid);
  CHECK(store.files["img"] == "fresh");
  CHECK(!store.exists("old"));
  CHECK(!store.exists("new"));
}

void recovery_restores_the_image_when_staged_is_unusable() {
  const auto invalid = [](const char*) { return false; };
  FakeStore store;
  store.files = {{"old", "previous"}, {"new", "garbage"}};
  desk::recoverInterruptedUpdate(store, "img", "new", "old", invalid);
  CHECK_MSG(store.exists("img") && store.files["img"] == "previous", "image was not restored from its backup");
  CHECK(!store.exists("new"));
}

void recovery_drops_a_backup_that_is_no_longer_needed() {
  const auto valid = [](const char*) { return true; };
  FakeStore store;
  store.files = {{"img", "current"}, {"old", "stale"}};
  desk::recoverInterruptedUpdate(store, "img", "new", "old", valid);
  CHECK(store.files["img"] == "current");
  CHECK(!store.exists("old"));
}

// ---- timers and Wi-Fi ownership --------------------------------------------------------------------------------

void cooldown_blocks_only_inside_its_window() {
  desk::ActionCooldown cooldown;
  CHECK(!cooldown.blocks(100));  // never started
  cooldown.start(1000, 1000);
  CHECK(cooldown.blocks(1000));
  CHECK(cooldown.blocks(1999));
  CHECK(!cooldown.blocks(2000));
  CHECK(!cooldown.blocks(2001));
}

void cooldown_does_not_come_back_after_a_long_idle() {
  desk::ActionCooldown cooldown;
  cooldown.start(1000, 1000);
  CHECK(!cooldown.blocks(2500));  // loop notices the expiry
  CHECK_MSG(!cooldown.blocks(2500u + 0x80000000u), "an expired cooldown swallowed an action 2^31 ms later");
  CHECK(!cooldown.blocks(2500u + 0xFFFFFF00u));  // nor around the 32-bit wrap
}

void cooldown_survives_the_counter_wrapping_mid_window() {
  desk::ActionCooldown cooldown;
  cooldown.start(0xFFFFFF00u, 1000);  // window straddles the wrap
  CHECK(cooldown.blocks(0xFFFFFF80u));
  CHECK(cooldown.blocks(0x00000010u));
  CHECK(!cooldown.blocks(0x00000300u));
}

void radio_refuses_entry_without_claiming_an_active_access_point() {
  desk::RadioLease lease;
  const desk::RadioEntryDecision decision = lease.enter(false, true);
  CHECK_MSG(decision == desk::RadioEntryDecision::REFUSE_ACTIVE_ACCESS_POINT,
            "Wi-Fi selection was allowed while another screen's access point was active");
  CHECK_MSG(!lease.ownsRadio(), "another screen's access point was claimed");
  const desk::RadioLease::ExitPlan plan = lease.onExit(true);
  CHECK_MSG(!plan.teardown, "another screen's access point would be torn down on exit");
  CHECK_MSG(!plan.restoreTuning, "radio tuning would be restored even though Desk mode did not change it");
}

void radio_uses_an_existing_station_without_claiming_it() {
  desk::RadioLease lease;
  const desk::RadioEntryDecision decision = lease.enter(true, false);
  CHECK(decision == desk::RadioEntryDecision::USE_CONNECTED_STATION);
  CHECK(!lease.ownsRadio());
  CHECK(!lease.onExit(true).teardown);

  desk::RadioLease stationAndAccessPoint;
  CHECK(stationAndAccessPoint.enter(true, true) == desk::RadioEntryDecision::USE_CONNECTED_STATION);
  CHECK(!stationAndAccessPoint.ownsRadio());
}

void radio_allows_selection_and_owns_the_connection_it_starts() {
  desk::RadioLease lease;
  const desk::RadioEntryDecision decision = lease.enter(false, false);
  CHECK(decision == desk::RadioEntryDecision::SELECT_NETWORK);
  CHECK(lease.ownsRadio());
  CHECK(lease.onExit(true).teardown);
  CHECK(!lease.onExit(false).teardown);  // driver already gone
}

void radio_tuning_is_restored_to_what_it_was() {
  desk::RadioLease lease;
  lease.enter(true, false);
  CHECK(!lease.onExit(true).restoreTuning);  // never tuned: nothing to undo
  lease.rememberTuning(0 /* power save off, as the web server leaves it */, false);
  const desk::RadioLease::ExitPlan plan = lease.onExit(true);
  CHECK(plan.restoreTuning);
  CHECK_MSG(plan.sleepMode == 0, "power save restored to %d, was 0", plan.sleepMode);
  CHECK(plan.autoReconnect == false);
  CHECK(!lease.onExit(false).restoreTuning);

  desk::RadioLease modem;
  modem.rememberTuning(2, true);
  CHECK(modem.onExit(true).sleepMode == 2);
  CHECK(modem.onExit(true).autoReconnect);
}

struct Test {
  const char* name;
  void (*run)();
};

#define TEST(fn) {#fn, fn}
const Test TESTS[] = {
    TEST(webhook_reply_is_read_under_a_byte_cap),
    TEST(webhook_trickled_reply_stops_at_the_deadline),
    TEST(webhook_malformed_url_is_not_logged),
    TEST(webhook_outcomes_never_call_an_unconfirmed_send_a_failure),
    TEST(version_trickle_stops_at_the_deadline),
    TEST(version_stops_reading_at_the_token_cap),
    TEST(version_cut_short_is_rejected),
    TEST(version_complete_reply_is_accepted_and_trimmed),
    TEST(version_error_status_is_rejected),
    TEST(image_download_succeeds_and_hashes),
    TEST(image_error_page_is_read_under_a_byte_cap),
    TEST(image_declared_length_over_the_cap_is_refused_unread),
    TEST(image_without_a_length_stops_at_the_byte_cap),
    TEST(image_trickle_stops_at_the_deadline),
    TEST(image_deadline_counts_time_spent_before_the_transfer),
    TEST(image_write_failure_fails_the_download),
    TEST(image_cut_short_by_the_network_fails),
    TEST(bmp_header_accepts_the_desk_image),
    TEST(bmp_header_extreme_dimensions_are_rejected_without_overflow),
    TEST(swap_failure_keeps_the_old_image),
    TEST(swap_success_replaces_the_image_and_leaves_no_backup),
    TEST(swap_with_no_previous_image_just_installs),
    TEST(swap_replaces_a_stale_backup),
    TEST(recovery_after_power_loss_between_the_renames),
    TEST(recovery_restores_the_image_when_staged_is_unusable),
    TEST(recovery_drops_a_backup_that_is_no_longer_needed),
    TEST(cooldown_blocks_only_inside_its_window),
    TEST(cooldown_does_not_come_back_after_a_long_idle),
    TEST(cooldown_survives_the_counter_wrapping_mid_window),
    TEST(radio_refuses_entry_without_claiming_an_active_access_point),
    TEST(radio_uses_an_existing_station_without_claiming_it),
    TEST(radio_allows_selection_and_owns_the_connection_it_starts),
    TEST(radio_tuning_is_restored_to_what_it_was),
};

}  // namespace

int main(const int argc, char** argv) {
  int ran = 0;
  for (const Test& test : TESTS) {
    if (argc > 1 && std::strcmp(argv[1], test.name) != 0) continue;
    currentTest = test.name;
    const int before = failures;
    test.run();
    ran++;
    std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", test.name);
  }
  if (ran == 0) {
    std::printf("no test named %s\n", argc > 1 ? argv[1] : "");
    return 2;
  }
  std::printf("%d test(s) run, %d check failure(s)\n", ran, failures);
  return failures == 0 ? 0 : 1;
}
