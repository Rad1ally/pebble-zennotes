# ZenNotes for Pebble

English | [日本語](README.ja.md)

Browse and read the notes on your [ZenNotes](https://zennotes.org) server ([znserver](https://github.com/ZenNotes/znserver)) from your Pebble watch. The app is read-only: it shows your vault, but never changes it.

This is an unofficial app. It is not made by, or affiliated with, the ZenNotes project.

For Pebble Time 2 (emery), tested with the Android Pebble app. Round watches (gabbro) are not supported yet.

## Features

- **Browse**: folders first (with icons), then notes, newest first. Also lists all notes by date (**Recent**) and notes grouped by frontmatter tag (**Tags**).
- **Search**: find notes by title with voice dictation.
- **Markdown**: shows headings, lists, checkboxes, quotes, callouts and tables. Wide tables scroll sideways.
- **Code blocks**: a code block shows its first three lines and its language. Tap it, or press SELECT, to open the full block with line numbers and syntax highlighting.
- **Links**: wikilinks (`[[Note]]`, `[[Note|label]]`) are shown in color. Tap one to open the note it links to, and press BACK to return. Links resolve the way Obsidian resolves them: by path, then a note with the same name in the same folder, then the note with that name and the shortest path.
- **Contents**: hold SELECT on a note to list its headings and links. Pick a heading to jump to it, or a link to open that note.
- **Images**: `![[image.png]]`, `![alt](URL)` and paths relative to the note. The phone downloads the image (PNG, JPEG, WebP, SVG and more), scales it to fit the screen, and reduces it to 64 colors before sending it to the watch.
- **Long notes**: a note over 32 KB is split into pages, with a "Continue reading" entry at the end of each page.
- **Offline**: the note list and the last 30 notes you opened are kept on the phone. They are shown when the server can't be reached.
- **Remembers your place**: the scroll position of each note and the last folder you opened.
- **Touch**: on Pebble Time 2, swipe and tap in lists, scroll notes, and swipe right to go back.
- **Status bar**: a Vim-style bar at the bottom shows the mode, button hints, your position and loading progress.
- **Settings**: theme (dark or light), text size, status bar, scroll indicator, and language (English or Japanese).

Hold SELECT in a list to reload the note list from the server.

## Requirements

- A ZenNotes server reachable from your phone, and an API token for it. Use HTTPS, or a VPN, if you read notes away from home.
- The Pebble app on Android.

## Setup

1. Install the app on your watch.
2. Open its settings in the Pebble app on your phone.
3. Enter your server URL and token, then save. Neither has a default value.
4. Open the app on your watch.

## Building from source

Install [`pebble-tool`](https://github.com/coredevices/pebble-tool) with [uv](https://docs.astral.sh/uv/), then the SDK:

```bash
uv tool install pebble-tool --python 3.13
pebble sdk install latest
```

Build, then install on the watch. Turn on the developer connection in the Pebble app first.

```bash
pebble build                                # -> build/pebble-zennotes.pbw
pebble install --phone <phone IP address>   # or: pebble login && pebble install --cloudpebble
```

The emulator (`pebble install --emulator emery`) works, but it has no Japanese font.

## How it works

- `src/c/main.c` runs on the watch and draws the lists, notes, code views and images.
- `src/pkjs/index.js` runs on the phone. It calls the ZenNotes API (`GET /api/notes`, `GET /api/notes/read?path=`, `GET /api/assets/raw`) with a Bearer token, and caches the note list. It also converts Markdown into display blocks and sends them to the watch in 2000-byte chunks.

## License

[MIT](LICENSE)
