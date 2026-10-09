# Changelog

## 1.3

- **Link previews** under messages: image thumbnails, page cards (title, description and picture
  from `og:` tags), YouTube cards (oEmbed), video and GIF cards. Tap to open.
- **Built-in MP4 player** (hardware decoder): pause, ±10 s, open in the browser.
- The viewer decodes big photos at reduced size; WebP everywhere.
- Setting to turn link previews off.

## 1.2

- **Image search** on Safebooru, Danbooru and Sankaku: thumbnail grid, pages, open in the viewer,
  **send to chat** (direct link, or re-uploaded for Sankaku's expiring links), **save to the Vita**
  (`ux0:picture/VitaIRC/`) and **favorites** with their own tab and local thumbnails.
- General content only by default; adult content is an opt-in setting with an 18+ confirmation.
  Tags that sexualise minors are always filtered.
- Optional Sankaku account in Settings.
- The image viewer opens **WebP** images too.

## 1.1

- **Free translation** with Google (no key needed); ChatGPT stays available as an option.
- **Image services**: Litterbox (default, no account), ImgBB (with expiration and delete links),
  Imgur, and **soju FILEHOST** (upload to your own bouncer). Upload progress in the top bar.
- **My recent uploads**: resend old links or delete ImgBB images.
- **IRCv3 chathistory**: missed messages from soju (or any server with `draft/chathistory`) for
  channels and private chats, and older messages when scrolling past the top.
- **Reactions** (`+draft/react` / `+draft/unreact`) shown under each message.
- **Emoji** drawn with a bundled monochrome font (Noto Emoji).
- **Day separators** (Today / Yesterday / date), nick completion with `@`, per-channel **mute**,
  **offline queue** (messages typed while disconnected are sent after reconnecting) and a daily
  **update check** against GitHub Releases.
- Fixed: a mention at the very end of a message ("hi nick") did not highlight.
- Fixed: messages replayed by a bouncer were logged with the day they arrived instead of their own.
- Fixed: messages sent from another client of the same bouncer opened a channel-like window.
- Messages to channel operators (`@#channel`) go to the channel window.

## 1.0 — first release

- Native IRC client for PS Vita: multiple networks, SSL/TLS, NickServ / SASL PLAIN / server password.
- Automatic login: uses your password via SASL (falling back to NickServ) and regains your nick
  if an old session still holds it.
- IRCv3 `server-time`, `multi-prefix` and `away-notify`; works great with soju and ZNC bouncers.
- Persistent per-channel history and session restore.
- ChatGPT translation of incoming and outgoing messages, per-channel language, persistent cache and usage counter.
- Imgur uploads with preview, camera capture, built-in PNG/JPG viewer.
- Quick replies, drafts, tap-a-message actions, search and jump to last mention.
- mIRC colors, highlight words, notification chime, ignore list and PM flood protection.
- Operator tools (op, voice, kick, ban), `/away`, invitations, network presets.
- English and Spanish interface.
