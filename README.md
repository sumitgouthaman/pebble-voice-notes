# Pebble Voice Notes

A Pebble watchapp for recording and browsing timestamped voice memos (`emery`, `basalt`, `chalk`, `diorite`, `aplite`).

## How It Works

- **Recording**: Select **+ Take Voice Note** at the top of the list to start a dictation session. If assigned to a watch Quick Launch button (**Settings > Quick Launch**), launching the app via that shortcut starts dictation immediately.
- **Note List**: Notes are listed newest-first under day headers (`Today`, `Yesterday`, `Sat, Nov 07, 2026`), showing the time and a truncated preview.
- **Reading**: Press `SELECT` on a note to open it full screen. `UP` and `DOWN` scroll the text; pressing past the top or bottom moves to the previous or next note.
- **Storage & Sync**:
  - The watch stores the 20 most recent notes in local flash (160 characters max per note, staying within Pebble's 4 KB app storage quota).
  - The phone companion (`localStorage` via PebbleKit JS) stores the full note history and reconciles with the watch on app launch.
- **Phone Settings & TTL**: Open **Voice Notes > Settings** in the Pebble mobile app to edit or delete notes, add text notes manually, toggle Quick Launch auto-record, or set the auto-cleanup TTL (`1 Day`, `3 Days`, `1 Week`, `2 Weeks`, `1 Month`, or `Keep Forever`). Expired notes are pruned automatically on app startup.

## Build & Test (CloudPebble)

1. In the Pebble mobile app, enable **Settings > Developer Mode** and toggle on **Developer Connection** (using CloudPebble / Rebble).
2. Build and install:
   ```bash
   pebble build
   pebble install --cloudpebble
   ```
3. Stream logs (or install and tail logs in one command):
   ```bash
   pebble logs --cloudpebble
   # or:
   pebble install --cloudpebble --logs
   ```

For direct Wi-Fi Developer Connection, replace `--cloudpebble` with `--phone <PHONE_IP>`.

## Layout

```text
├── package.json          # App metadata, capabilities, messageKeys, platforms
├── src/c/
│   ├── main.c            # App init and Quick Launch timer
│   ├── model.h / .c      # Flash storage, TTL cleanup, day section grouping
│   ├── dictation.h / .c  # DictationSession wrapper
│   ├── ui_menu.h / .c    # Main MenuLayer
│   ├── ui_detail.h / .c  # Full-screen note view and prev/next navigation
│   └── comm.h / .c       # AppMessage sync and staging
└── src/pkjs/
    └── index.js          # Phone storage, sync reconciliation, settings webview
```
