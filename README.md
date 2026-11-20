# LeagueVox

League of Legends team voice chat only has push-to-talk, with no voice activity option. LeagueVox adds voice activity: it listens to your mic and holds your push-to-talk key while you speak, so you can talk without pressing anything.

It's a tiny Windows tray app. It only listens while a game is running and starts with Windows.

## Setup

1. Download `LeagueVox.exe` from [Releases](../../releases/latest) and run it. If Windows warns about it, click **More info → Run anyway** (the exe isn't signed).
2. Click the tray dot (bottom-right, next to the clock) → **Edit config**. Set `talk_key`, e.g. `talk_key=F11`, and save.
3. In League's voice settings, set push-to-talk to that same key.

Optional: set `mute_key` (e.g. `F12`) for a button that toggles LeagueVox on and off.

## Tray dot

Dark grey: no talk key set · Grey: waiting for a game · Green: listening · Red: talking · Purple: muted

Hover over it to see your mic level.

## Settings

Saved changes apply instantly.

| Setting | Default | |
|---|---|---|
| `talk_key` | none | Key held while you talk. Use one that doesn't type (F-keys, `PAUSE`, `INSERT`). |
| `mute_key` | none | Toggles mute. Can include `CTRL+`, `ALT+`, `SHIFT+`, e.g. `CTRL+M`. |
| `mute_beep` | `1` | Beep on mute and unmute. |
| `mode` | `hold` | `hold`, or `tap` for toggle-style push-to-talk. |
| `device` | default mic | Part of your mic's name, e.g. `HyperX`. |
| `open_db` | `-42` | How loud counts as talking. Raise it if noise sets it off; lower it if speech gets missed. |
| `close_db` | `-48` | Level that counts as stopped talking. |
| `attack_ms` | `30` | Sound needed before the key goes down. |
| `release_ms` | `400` | Silence needed before the key is released. |
| `processes` | `League of Legends.exe` | Processes that turn it on. |
| `poll_ms` | `2000` | How often to check for League, in ms. |

Key names: `F1`–`F24`, `A`–`Z`, `0`–`9`, `NUMPAD0`–`9`, `SPACE`, `TAB`, `INSERT`, `DELETE`, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, arrow keys (`UP` etc.), `PAUSE`, `SCROLLLOCK`, `MOUSE3`–`5` (talk key only), or hex like `0x7C`.

## Uninstall

Tray dot → **Exit**, then delete the exe and `LeagueVox.ini`. Remove it from Task Manager → Startup apps.

## Build

```
gcc -O2 -s -municode -mwindows -o LeagueVox.exe LeagueVox.c -lole32 -lshell32 -luser32 -lgdi32 -ladvapi32 -lm
```

Not affiliated with Riot Games. Use at your own risk.
