# MIDI Router

An ultra-low-latency, zero-dependency macOS CoreMIDI routing daemon compiled natively for Apple Silicon (ARM64).

Directly bridges USB MIDI between hardware instruments without needing a DAW, heavy background applications, or outdated Intel-era utilities (such as MidiPipe or MIDI Patchbay).

Designed specifically for headless, rock-solid studio rigs connected entirely via class-compliant USB directly into a Mac.

---

## Key Features

- **Multi-Rule Routing Engine**: Route multiple hardware sources to single or multiple destinations simultaneously.
- **Real-Time Clock & Transport Filtering**: Selectively strip System Real-Time messages (`0xF8` Clock, `0xFA` Start, `0xFB` Continue, `0xFC` Stop, `0xFE` Active Sensing) on controller routes so hardware sequencers retain master clock authority without sync conflicts.
- **Universal Hardware Support**: Works with **any** class-compliant USB MIDI device recognized by macOS (Elektron, Arturia, Moog, Roland, Korg, Novation, Teenage Engineering, Focusrite, etc.).
- **Zero Overhead**: Native C compiled for Apple Silicon with direct CoreMIDI buffer passing. Uses **0.0% CPU** and ~6 MB RAM.
- **Headless macOS LaunchAgent (Start on Boot)**: Runs silently in the background at login/boot with zero UI.
- **Auto-Reconnect (Hot-Plug & Sleep Resilient)**: Endpoints are matched dynamically by string name. When devices are powered off/on, unplugged/replugged, or when the Mac wakes from sleep, connections re-establish within milliseconds.
- **Live MIDI Activity Monitor**: Built-in real-time packet monitor to watch notes, CC, channel targeting, and transport events live in your terminal.

---

## Active Studio Configuration

By default, `midi-router` manages configuration via `~/.config/midi-router/routes.conf`:

```ini
# Rule 1: DT2 -> DN2 (Master Clock, Transport & Channel Data Bridge)
# Digitakt II acts as master hardware brain. Clock and transport pass through.
route Digitakt -> Digitone

# Rule 2: KeyStep -> DT2 & DN2 (Controller Fan-Out for Auto Channels)
# Broadcasts to both units with real-time clock/transport filtered out.
# Switch channels on KeyStep:
#   - Channel 14 -> DT2 Auto Channel (plays active sampler track)
#   - Channel 10 -> DN2 Auto Channel (plays active synth track)
route KeyStep -> Digitakt, Digitone filter-realtime
```

### How the Rig Operates:
1. **Digitakt II $\rightarrow$ Digitone II**:
   - Master MIDI Clock (`0xF8`), Transport (`0xFA` Start / `0xFC` Stop), and channel data flow from DT2 to DN2.
   - DT2 acts as the master hardware brain and clock source.
2. **KeyStep $\rightarrow$ DT2 & DN2 (Fan-Out & Filtering)**:
   - When the KeyStep is connected, incoming notes, velocity, pitch bend, aftertouch, and modulation strips are broadcast to **both** units.
   - Internal KeyStep clock and transport controls are filtered out so they never interfere with DT2's master sequencer.
   - **Channel 14**: Targets DT2's Auto Channel (instantly plays whichever sample track is currently selected on DT2).
   - **Channel 10**: Targets DN2's Auto Channel (instantly plays whichever synth track is currently selected on DN2).

---

## CLI Commands

You can run `midi-router` from any terminal:

### Check Status & Active Routes
```bash
midi-router --status
```
Example output:
```text
[STATUS] Background service 'com.stevenrobinson.midi-router' is RUNNING (PID 77952)
Config file: /Users/stevenrobinson/.config/midi-router/routes.conf
Log file:    /Users/stevenrobinson/Library/Logs/midi-router.log

Configured Routes (2):
  1. DT2 -> DN2:      'Digitakt' [ONLINE] -> 'Digitone' [ONLINE] (Pass-Through)
  2. KeyStep -> Both: 'KeyStep' [WAITING] -> ['Digitakt', 'Digitone'] (Clock Filtered)
```

### Live MIDI Monitor
Inspect incoming MIDI data in real time to verify note numbers, velocities, and Auto Channel targeting:
```bash
midi-router -v
```
*(Press `Ctrl+C` when done. The background service continues running.)*

### Stream Background Logs
```bash
tail -f ~/Library/Logs/midi-router.log
```

### List All Connected Devices
```bash
midi-router --list
```

### Install / Restart Background Service
```bash
midi-router --install
```

### Uninstall / Stop Background Service
```bash
midi-router --uninstall
```

---

## Configuration Syntax (`routes.conf`)

Edit `~/.config/midi-router/routes.conf` to add or modify rules:

```ini
route <SourcePattern> -> <DestPattern1>, <DestPattern2>, ... [options]
```

### Options:
- `filter-realtime`: Strips System Real-Time messages (`0xF8` Clock, `0xFA` Start, `0xFB` Continue, `0xFC` Stop, `0xFE` Active Sensing). Essential for hardware keyboards/sequencers with internal clocks.

---

## Elektron Hardware Settings Reference

### Digitakt II (Master Sequencer & Clock Transmitter):
- `[SETTINGS] > SYSTEM > USB CONFIG`: Set to **USB MIDI**.
- `[SETTINGS] > MIDI CONFIG > PORT CONFIG`:
  - `OUT PORT FUNC`: **MIDI** (or **MIDI+USB**)
  - `OUTPUT TO`: **USB** (or **MIDI+USB**)
  - `CLOCK SEND`: **Checked**
  - `TRANSPORT SEND`: **Checked**
- `[SETTINGS] > MIDI CONFIG > CHANNELS`:
  - `AUTO CHANNEL`: Set to **14** (default)

### Digitone II (Synth Receiver):
- `[SETTINGS] > SYSTEM > USB CONFIG`: Set to **USB MIDI**.
- `[SETTINGS] > MIDI CONFIG > PORT CONFIG`:
  - `IN PORT FUNC`: **MIDI** (or **MIDI+USB**)
  - `INPUT FROM`: **USB** (or **MIDI+USB**)
  - `CLOCK RECEIVE`: **Checked**
  - `TRANSPORT RECEIVE`: **Checked**
- `[SETTINGS] > MIDI CONFIG > CHANNELS`:
  - `AUTO CHANNEL`: Set to **10** (default)

---

## Building from Source

```bash
git clone https://github.com/Steven-Robinson/midi-router.git
cd midi-router
make
make install   # Installs binary to ~/.local/bin/midi-router
```
