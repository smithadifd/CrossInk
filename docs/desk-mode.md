---
title: Desk Mode
nav_order: 7.7
---

# Desk Mode

Desk mode turns the reader into a small status display with a few action buttons. A server on your
network renders a 1-bit, full-screen BMP; the reader shows it, refreshes it when it changes, and sends
a webhook when you tap a zone or press a button.

Desk mode needs Wi-Fi and a configuration file on the SD card. It is built for a reader that sits on a
desk on USB power, but it also works on battery (see [Power](#power)).

## Setup

1. Copy [`desk.json.example`](./desk.json.example) to `/.crosspoint/desk.json` on the SD card and edit the URLs.
2. On the reader, open **Settings > System > Device > Desk mode** and pick a Wi-Fi network if prompted.

If the file is missing or invalid, Desk mode shows a message naming the path and the expected fields instead
of starting.

### `desk.json`

```json
{
  "image_url": "http://192.168.1.50:8080/desk/status.bmp",
  "version_url": "http://192.168.1.50:8080/desk/version",
  "webhook_url": "http://192.168.1.50:8080/desk/action",
  "poll_s": 60,
  "full_refresh_every": 20
}
```

| Field | Required | Default | Meaning |
| --- | --- | --- | --- |
| `image_url` | yes | | `http://` or `https://` URL of the screen image. |
| `webhook_url` | yes | | `http://` or `https://` URL that receives actions. |
| `version_url` | no, but strongly recommended | none | URL of a short text value that changes when the image changes. When set, the image is only downloaded and redrawn when this value changes. It also lets the reader notice an unreachable server within a few seconds, before trying the larger image download. |
| `poll_s` | no | `60` | Seconds between checks. Clamped to 10-3600. |
| `full_refresh_every` | no | `20` | Image updates use a fast refresh, so a server that redraws every minute does not flash the panel. The first image after entering Desk mode, and every Nth image that actually changed, use a half refresh to clear ghosting. Only image changes are counted: unchanged polls, action messages and the offline label do not. Left (force refresh) always gives a half refresh. |

URL schemes are case-insensitive. A UTF-8 byte order mark at the start of `desk.json` is ignored.

HTTPS URLs are accepted, but the reader does **not verify the server certificate** for any Desk mode request
(image, version or webhook). HTTPS therefore encrypts the traffic but does not authenticate the server, and
gives no protection against an attacker on your network. Use plain `http://` on a trusted LAN.

## Controls

Desk mode always draws in portrait (480 x 800), whatever orientation the reader last used, and restores the
previous orientation when you leave.

The bottom 240 pixels of the screen are a 2x2 grid of touch cells, each half the screen wide and 120 pixels
tall. The server image should draw matching boxes there, inset 8 px inside each cell.

```text
+------------+------------+
| video_call |   focus    |
+------------+------------+
|  evening   |  restore   |
+------------+------------+
```

| Action | Touch cell | Button |
| --- | --- | --- |
| `video_call` | top left | Confirm |
| `focus` | top right | Up (side button) |
| `evening` | bottom left | Down (side button) |
| `restore` | bottom right | Right |
| Force refresh (re-download and half refresh) | | Left |
| Exit | | Back, or the left-edge swipe |

Touch rectangles on a 480 x 800 screen (end values exclusive):

| Action | x | y |
| --- | --- | --- |
| `video_call` | 8-232 | 568-672 |
| `focus` | 248-472 | 568-672 |
| `evening` | 8-232 | 688-752 |
| `restore` | 248-472 | 688-752 |

Each rectangle is its cell inset by 8 px; right and bottom edges are exclusive. The bottom row stops at y = 752,
where the screen's bottom-edge gesture band (the lowest 6%, 48 px) begins. The server draws exactly these
rectangles (desk-panel clips its bottom boxes at 752 too), so every drawn box is tappable edge to edge. Taps
between the rectangles do nothing. The left-edge Back swipe is a
horizontal swipe, so it does not conflict with taps inside the left cells.

The Home key and bottom-edge Home swipe also leave Desk mode, like on any other screen. Depending on the
Power button setting, Power may also act as Confirm.

After an action, a short message appears above the grid for a couple of seconds:

- `Video call - sent`: the server answered with a 2xx status.
- `Video call - no reply, check lights`: no HTTP reply arrived within 3 seconds, but the server still accepts
  connections. The request may or may not have gone through, so check the result before pressing again.
- `Video call failed (HTTP 500)`, `failed (unreachable)` or `failed (no Wi-Fi)`: the action was not delivered.

Further actions are ignored for about one second after each one, and while one is being sent. After a sent
action the reader checks for a new image about 2 seconds later and again about 6 seconds later, so a server that
redraws in response shows the result quickly.

## Server expectations

### Image

`GET image_url` returns HTTP 200 with a BMP file: uncompressed, 1 bit per pixel, exactly 480 x 800 (portrait),
at most 128 KB. The reader streams it to `/.crosspoint/desk/status.new` and accepts it only if the header
matches and the file is at least as long as the header says (offset to pixel data plus 60 bytes per row times
800 rows). Only then is `status.bmp` replaced, so a failed, truncated or wrong-format download never blanks the
screen. If power is lost during the swap, the complete `status.new` is promoted the next time Desk mode opens.

Each socket operation times out after 3 seconds and one download is abandoned after 15 seconds in total.

Without `version_url`, the reader downloads the image every `poll_s` seconds and redraws only when its
contents differ from the previous download.

### Version

`GET version_url` returns a short plain-text body (an ISO timestamp, a counter, or a hash; only the first 128
bytes are used and surrounding whitespace is ignored) that changes whenever the image changes. Respond with
HTTP 200. Requests time out after 3 seconds. Any other status is treated as the server being unreachable. An
empty body carries no information, so the image is downloaded on every check.

### Webhook

`POST webhook_url` with `Content-Type: application/json` and a body of exactly one field:

```json
{"action":"video_call"}
```

`action` is one of `video_call`, `focus`, `evening`, `restore`. Reply with any 2xx status to acknowledge; the
response body is ignored. Any other status is shown as a failure. If no reply arrives within 3 seconds the
reader shows "no reply, check lights" or "failed (unreachable)" as described above. The reader does not retry.

## Offline behaviour

If a check fails or Wi-Fi drops, the last good image stays on screen with a small "offline - retrying" label in
the top-right corner. While offline the reader backs off: the wait before each retry doubles, up to 300 seconds
(or `poll_s` if that is larger), and returns to `poll_s` after the first success. Pressing Left forces an
immediate check, and a check also runs as soon as Wi-Fi reconnects.

A failed check blocks the screen only briefly: each request gives up after 3 seconds, and the next check is
scheduled after the failed one has finished. Setting `version_url` keeps this short, because an unreachable
server is detected by the small version request before the image download is attempted.

## Power

- On USB power the reader does not auto-sleep while Desk mode is open.
- On battery the normal sleep timeout applies, and Wi-Fi is turned off when the reader sleeps. There is no
  timer wake: press Power to wake it, then open Desk mode again.
- Leaving Desk mode restores the Wi-Fi power-save and auto-reconnect settings it changed, and turns Wi-Fi off if
  Desk mode turned it on.

## Troubleshooting

- **"Config not usable"**: the file at `/.crosspoint/desk.json` is missing, is not valid JSON, or lacks a
  required URL. The line under the path names the problem.
- **"Waiting for image..." stays up**: the first download has not succeeded. Check `image_url` from another
  device, confirm it is a 480 x 800 1-bit BMP, and look at the serial log (tag `DESK`).
- **"failed (unreachable)"**: the webhook server did not answer or accept a connection within 3 seconds, or Wi-Fi is
  down.
