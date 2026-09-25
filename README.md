# MIDI Router for Elektron Digitakt II & Digitone II

An ultra-low-latency, zero-dependency macOS CoreMIDI routing daemon compiled natively for Apple Silicon (ARM64).

Designed specifically to bridge USB MIDI between modern Elektron devices (Digitakt II -> Digitone II) without needing a DAW, old Intel-only utilities (like MidiPipe or MIDI Patchbay), or heavy background apps.

---

## Current Status

The background service is **installed and active**:
- **Source**: `Elektron Digitakt II`
- **Destination**: `Elektron Digitone II`
- **Autostart**: Enabled on every login/boot via `launchd`
- **Latency**: Sub-millisecond (direct CoreMIDI buffer forwarding)
- **CPU / RAM usage**: 0.0% CPU, ~6 MB RAM

---

## Quick Commands

You can run `midi-router` from any terminal:

### Check Status & Recent Logs
```bash
midi-router --status
```

### View Live Logs (connection / disconnection events)
```bash
tail -f ~/Library/Logs/midi-router.log
```

### List Detected MIDI Devices
```bash
midi-router --list
```

### Run Interactively in Foreground (with live MIDI monitor)
If you want to watch notes, CC messages, and transport controls in real-time on your screen:
```bash
midi-router -v
```
*(Press `Ctrl+C` when done. The background service will continue running.)*

### Bi-directional Mode (Digitakt <-> Digitone)
If you ever want both machines to send MIDI to each other simultaneously:
```bash
midi-router --bidirectional --install
```

### Uninstall / Disable
If you ever want to stop and remove the background service:
```bash
midi-router --uninstall
```

---

## Elektron Hardware Configuration Tips

To ensure MIDI flows between your Digitakt II and Digitone II:

1. **USB Mode**:
   - On both machines: Go to `[SETTINGS] > SYSTEM > USB CONFIG`.
   - Set to **USB MIDI** (not Overbridge, unless you want Overbridge audio streaming enabled alongside MIDI).

2. **MIDI Ports (Digitakt II - Transmitter)**:
   - Go to `[SETTINGS] > MIDI CONFIG > PORT CONFIG`.
   - `OUT PORT FUNC`: Set to **MIDI** (or **MIDI+USB**).
   - `OUTPUT TO`: Ensure **USB** (or **MIDI+USB**) is selected.
   - `CLOCK SEND`: Enable if you want Digitakt II to control the tempo of Digitone II.
   - `TRANSPORT SEND`: Enable if you want Digitakt's Play/Stop buttons to start/stop Digitone II.

3. **MIDI Ports (Digitone II - Receiver)**:
   - Go to `[SETTINGS] > MIDI CONFIG > PORT CONFIG`.
   - `IN PORT FUNC`: Set to **MIDI** (or **MIDI+USB**).
   - `INPUT FROM`: Ensure **USB** (or **MIDI+USB**) is selected.
   - `CLOCK RECEIVE`: Enable to sync tempo with Digitakt.
   - `TRANSPORT RECEIVE`: Enable to sync Play/Stop with Digitakt.

4. **MIDI Channels**:
   - Go to `[SETTINGS] > MIDI CONFIG > CHANNELS`.
   - Verify the MIDI tracks on your Digitakt II are assigned to the MIDI channels of the synth tracks on your Digitone II (e.g. Tracks 1-4 on Digitone set to Channels 1-4).
