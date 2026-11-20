# LeagueVox

Voice-activated push-to-talk for League of Legends. When League is running, LeagueVox listens to your mic and holds your push-to-talk key down while you speak. When League isn't running it does nothing and doesn't open the mic.

It runs from the system tray and starts with Windows.

## Download

Get `LeagueVox.exe` from the [Releases](../../releases/latest) page. It's a single file with no installer; put it in any folder.

Windows may say "Windows protected your PC" because the exe isn't signed. Click **More info → Run anyway**.

## Setup

No keys are bound out of the box, so LeagueVox does nothing until you set one.

1. Run `LeagueVox.exe`. A dot appears in the system tray (bottom-right, next to the clock; check the **^** arrow if it's hidden), and `LeagueVox.ini` is created next to the exe.
2. Click the dot, choose **Edit config**, and set `talk_key` to a key you don't use in League, e.g. `talk_key=F11`. Save. The change applies right away.
3. In League, open Settings and set voice chat to push-to-talk. Click the push-to-talk bind and press that same key.
4. Optional: set `mute_key` (e.g. `mute_key=F12`) to get a button that mutes and unmutes LeagueVox.

## Tray icon

| Colour | Meaning |
|---|---|
| Dark grey | No talk key set |
| Grey | Waiting for League |
| Green | Listening |
| Red | Talking (talk key held) |
| Purple | Muted |
| Orange | Mic error, retrying |

Hover over the icon to see your live mic level. Click it for the menu:

- **Mute**: same as pressing the mute key.
- **Test mic now**: listen even when League isn't running, to tune your settings.
- **Edit config**: opens `LeagueVox.ini` in Notepad.
- **Reload config** / **Exit**

## Settings (`LeagueVox.ini`)

Changes apply as soon as you save the file. No restart needed.

| Setting | Default | What it does |
|---|---|---|
| `processes` | `League of Legends.exe` | Comma-separated process names that turn listening on. `League of Legends.exe` is an actual game; add `LeagueClientUx.exe` to also listen in the client/lobby. |
| `talk_key` | *(empty)* | Key held down while you talk. Bind the same key to push-to-talk in League. Empty = LeagueVox stays off. |
| `mode` | `hold` | `hold` = key held while you talk (push-to-talk). `tap` = one tap when you start talking, one when you stop (for toggle-to-talk apps). |
| `mute_key` | *(empty)* | Press once to mute, press again to unmute. While muted, LeagueVox never presses the talk key. Empty = no mute key. |
| `mute_beep` | `1` | `1` = short beep on mute (low) and unmute (high), so you can tell in-game. `0` = silent. |
| `device` | *(empty)* | Microphone. Empty = Windows default, or part of the device name (e.g. `Yeti`, `HyperX`). |
| `open_db` | `-42` | Loudness (dBFS, 0 = max) that counts as talking. Raise toward `-30` if background noise sets it off; lower toward `-50` if quiet speech is missed. |
| `close_db` | `-48` | Level must drop below this before the key starts to release. Keep it a few dB under `open_db`. |
| `attack_ms` | `30` | Milliseconds of sound before the key goes down. Filters out desk taps and clicks. |
| `release_ms` | `400` | Milliseconds of quiet before the key is released. Keeps it held between words. |
| `poll_ms` | `2000` | How often to check whether League is running, in milliseconds. |

### Key names

`talk_key` accepts: `F1`–`F24`, `A`–`Z`, `0`–`9`, `NUMPAD0`–`NUMPAD9`, `SPACE`, `TAB`, `CAPSLOCK`, `LSHIFT`, `RSHIFT`, `LCTRL`, `RCTRL`, `LALT`, `RALT`, `INSERT`, `DELETE`, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `SCROLLLOCK`, `PAUSE`, `GRAVE` (the `` ` `` key), `MOUSE3`, `MOUSE4`, `MOUSE5`, or a hex virtual-key code like `0x7C`.

`mute_key` accepts the same names except the mouse buttons, optionally with `CTRL+`, `ALT+`, `SHIFT+`, `WIN+` in front (e.g. `CTRL+M`, `CTRL+SHIFT+F1`).

### Tips

- **Use keys that don't type anything.** F-keys, `PAUSE` or `INSERT` work well. A letter, Space or Shift as the talk key *will* type into chat while you speak.
- **League may not accept F13–F24.** Most keyboards don't have them, and League's keybind menu may ignore them. Pick a key that's on your keyboard so you can bind it by pressing it.
- **The mute key is taken by LeagueVox.** The game won't see it, so don't pick one you use in League.
- **The mute key can't be the same key as the talk key** (with no modifier). LeagueVox ignores that.
- LeagueVox always starts unmuted.

## Startup and uninstalling

The first time you run it, LeagueVox adds itself to Windows startup (`HKCU\...\CurrentVersion\Run`). It then starts silently in the tray each time you log in. When League isn't running it only checks every couple of seconds whether League has started.

To remove it: click the tray dot → **Exit**, then delete the exe and `LeagueVox.ini`. The startup entry is then left pointing at nothing; remove it from Task Manager → Startup apps if you like.

## Disclaimer

LeagueVox is not made or endorsed by Riot Games. It only presses the push-to-talk key you choose, but it is still a third-party tool sending input while League runs. Use it at your own risk.

## Building

With MinGW-w64:

```
gcc -O2 -s -municode -mwindows -o LeagueVox.exe LeagueVox.c -lole32 -lshell32 -luser32 -lgdi32 -ladvapi32 -lm
```
