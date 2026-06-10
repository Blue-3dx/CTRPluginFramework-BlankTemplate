# vc3ds — 3DS Voice Chat Plugin

In-game voice chat for Nintendo 3DS using **CTRPluginFramework** and **libopus**.

```
Plugin menu
├── Connect to VC          ← toggle: opens TCP session, starts voice thread
├── Mute                   ← toggle: silences your mic (VAD still runs)
├── Voice Status Overlay   ← toggle: on-screen user list (green = talking)
│                             ⚠️  Disable on Old 3DS if you see FPS drops
├── Leave Room
├── Status                 ← popup: connection / room / talking info
├── VC Rooms/
│   ├── Room 0 (x/5)       ← tap to join
│   ├── Room 1 (x/5)
│   ├── ...
│   └── Refresh Rooms
└── Settings/
    ├── Set Server Address  ← default: 192.168.1.100
    └── Set Username
```

---

## Server setup (Linux / aarch64)

```bash
# Install from the GitHub Actions artifact, or build locally:
cmake -S server -B server/build -DCMAKE_BUILD_TYPE=Release
cmake --build server/build

./server/build/vc3ds-server
# optional flags: --tcp-port 7001  --udp-port 7002  --rooms 8
```

Open firewall ports **TCP 7001** and **UDP 7002**.

---

## Building the plugin locally

Requires [devkitPro](https://devkitpro.org) + libctrpf.

```bash
# Install 3ds-libopus (or let the workflow build it)
dkp-pacman -S 3ds-libopus

make
```

Output: `vc3ds.3gx` — place it in `/luma/plugins/` on your SD card.

---

## How it works

| Layer | Detail |
|---|---|
| Transport | TCP 7001 (control), UDP 7002 (voice) |
| Codec | Opus 16 kHz mono, 20 ms frames, ~24 kbps |
| Mic | MICU at 16360 Hz → Opus encoder |
| Speaker | NDSP channel 23 (away from game channels) |
| VAD | RMS amplitude threshold (600 default) |
| OSD | CTRPF OSD callback; talking user names turn green |
| Rooms | 8 rooms × 5 users; relay server forwards UDP packets |

---

## Known limitations

- **Mic conflict**: if the game uses the microphone (Nintendogs etc.) the plugin cannot capture audio.
- **SOC conflict**: the plugin initialises the SOC (socket) service; on the rare game that also uses SOC you may see a connection failure.
- **NDSP**: ctrulib's `ndspInit` is reference-counted, so sharing channel 23 with the game is safe.
- **Server address** is stored in RAM only — set it every session from *Settings → Set Server Address*, or change the default in `Includes/VoiceChat.hpp`.
