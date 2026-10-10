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
| `version_url` | no | none | URL of a short text value that changes when the image changes. When set, the image is only downloaded and redrawn when this value changes. |
| `poll_s` | no | `60` | Seconds between checks. Clamped to 10-3600. |
| `full_refresh_every` | no | `20` | Every Nth screen update uses a half refresh to clear ghosting; the rest use fast refresh. The first paint is always a half refresh. |

HTTPS URLs are accepted but the server certificate is not verified, so prefer plain `http://` on a trusted LAN.

## Controls

The bottom 240 pixels of the screen are a 2x2 grid of touch cells, each half the screen wide and 120 pixels
tall. The server image should draw matching boxes there.

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

The Home key and bottom-edge Home swipe also leave Desk mode, like on any other screen. Depending on the
Power button setting, Power may also act as Confirm.

After an action is sent, a short message ("Video call - sent" or "Video call failed (HTTP 500)") appears above
the grid for a couple of seconds. After a successful action the reader checks for a new image after about two
seconds, so a server that redraws in response shows the result quickly.

## Server expectations

### Image

`GET image_url` returns a BMP file, 1 bit per pixel, 480 x 800 (portrait). The reader writes it to
`/.crosspoint/desk/status.bmp` through a temporary `.part` file and only replaces the previous image after the
new one parses as a valid BMP, so a failed or truncated download never blanks the screen. A smaller image is
centred; a larger one is scaled.

Without `version_url`, the reader downloads the image every `poll_s` seconds and redraws only when its
contents differ from the previous download.

### Version

`GET version_url` returns a short plain-text body (an ISO timestamp, a counter, or a hash; only the first 128
bytes are used and surrounding whitespace is ignored) that changes whenever the image changes. Respond with
HTTP 200. Requests time out after 3 seconds. Any other status is treated as the server being unreachable.

### Webhook

`POST webhook_url` with `Content-Type: application/json` and a body of exactly one field:

```json
{"action":"video_call"}
```

`action` is one of `video_call`, `focus`, `evening`, `restore`. Reply with any 2xx status to acknowledge; the
response body is ignored. Any other status, or no response within 3 seconds, is shown as a failure. The reader
does not retry.

## Offline behaviour

If a check fails or Wi-Fi drops, the last good image stays on screen with a small "offline - retrying" label in
the top-right corner. The reader keeps retrying every `poll_s` seconds and clears the label on the next success.

A check that has to download an image from an unresponsive server can block the screen for tens of seconds
(the download timeout is shared with the rest of the firmware). Setting `version_url` avoids this, because the
quick version request detects an unreachable server first.

## Power

- On USB power the reader does not auto-sleep while Desk mode is open.
- On battery the normal sleep timeout applies, and Wi-Fi is turned off when the reader sleeps. There is no
  timer wake: press Power to wake it, then open Desk mode again.
- Leaving Desk mode turns Wi-Fi off if Desk mode turned it on.

## Troubleshooting

- **"Config not usable"**: the file at `/.crosspoint/desk.json` is missing, is not valid JSON, or lacks a
  required URL. The line under the path names the problem.
- **"Waiting for image..." stays up**: the first download has not succeeded. Check `image_url` from another
  device and look at the serial log (tag `DESK`).
- **"Failed (no response)"**: the webhook did not answer within 3 seconds or Wi-Fi is down.
