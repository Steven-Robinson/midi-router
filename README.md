# MIDI Router

An ultra-low-latency, zero-dependency macOS CoreMIDI routing daemon built natively for Apple Silicon (ARM64).

Directly routes MIDI between USB devices without needing a DAW, heavy background applications, or outdated Intel-era utilities (such as MidiPipe or MIDI Patchbay).

---

## Highlights

- **Universal Hardware Support**: Works with **any** class-compliant USB MIDI device recognized by macOS—including grooveboxes, synthesizers, drum machines, MIDI keyboards, pad controllers, and USB-to-DIN MIDI interfaces (Elektron, Arturia, Moog, Roland, Korg, Novation, Teenage Engineering, Focusrite, etc.).
- **Zero Overhead**: Native C compiled for Apple Silicon with direct CoreMIDI buffer passing. Uses **0.0% CPU** and ~6 MB RAM.
- **Starts on Boot**: Easily installs as a persistent macOS `launchd` service that starts automatically on login/boot and stays alive.
- **Auto-Reconnect (Hot-Plug Resilient)**: Automatically detects when devices are powered off/on or unplugged/replugged, re-establishing the MIDI connection within milliseconds.
- **Pure MIDI 1.0 Transparent**: Forwards all Notes, CC, Pitch Bend, Aftertouch (Channel & Polyphonic), Program Changes, Clock, Transport (Start/Stop/Continue), Song Position, and SysEx byte-for-byte.
- **Live MIDI Monitor**: Includes a built-in real-time packet monitor to watch notes, CC, and transport events live in your terminal.

---

## Quick Start

### 1. List Available MIDI Devices
See all inputs (sources) and outputs (destinations) currently connected to your Mac:
```bash
midi-router --list
```

### 2. Route Any Devices
Specify devices by name or partial name (case-insensitive):
```bash
# Example: Route an Arturia Keystep to a Korg Minilogue
midi-router -s "Keystep" -d "Minilogue"

# Example: Route an Elektron Digitakt II to an Elektron Digitone II
midi-router -s "Digitakt" -d "Digitone"
```
*(By default, `--source "Digitakt"` and `--dest "Digitone"` are used if no flags are specified.)*

### 3. Bi-directional Routing
To route both ways simultaneously (Source $\leftrightarrow$ Destination):
```bash
midi-router -s "Digitakt" -d "Digitone" --bidirectional
```

---

## Background Service (Start on Boot)

You can install `midi-router` as a native macOS background LaunchAgent with a single command. It will run silently in the background and auto-start every time your Mac boots:

```bash
# Install with defaults (Digitakt -> Digitone)
midi-router --install

# Or install with custom devices
midi-router -s "Keystep" -d "Digitone" --install

# Or install in bidirectional mode
midi-router -s "Digitakt" -d "Digitone" --bidirectional --install
```

### Manage the Background Service

- **Check status & recent logs:**
  ```bash
  midi-router --status
  ```
- **Stream live connection logs:**
  ```bash
  tail -f ~/Library/Logs/midi-router.log
  ```
- **Uninstall / Stop the background service:**
  ```bash
  midi-router --uninstall
  ```

---

## Live MIDI Activity Monitor

To inspect incoming MIDI data in real time (great for verifying whether notes, CC knobs, or clock are transmitting):

```bash
# Monitor notes, CC, pitch bend, and transport
midi-router -v

# Full monitor including high-frequency clock ticks
midi-router -vv
```
*(Press `Ctrl+C` to exit the monitor; any running background service will continue unaffected.)*

---

## Example Hardware Setup: Elektron Digitakt II & Digitone II

To route between an Elektron Digitakt II (transmitter/sequencer) and Digitone II (synth receiver):

1. **USB Config** (on both machines):
   - Navigate to `[SETTINGS] > SYSTEM > USB CONFIG`.
   - Set to **USB MIDI**.

2. **Digitakt II (Transmitter)**:
   - Navigate to `[SETTINGS] > MIDI CONFIG > PORT CONFIG`.
   - `OUT PORT FUNC`: Set to **MIDI** (or **MIDI+USB**).
   - `OUTPUT TO`: Set to **USB** (or **MIDI+USB**).
   - `CLOCK SEND`: Enable if you want Digitakt II to control the tempo of Digitone II.
   - `TRANSPORT SEND`: Enable if you want Digitakt's Play/Stop buttons to start and stop the Digitone II sequencer.

3. **Digitone II (Receiver)**:
   - Navigate to `[SETTINGS] > MIDI CONFIG > PORT CONFIG`.
   - `IN PORT FUNC`: Set to **MIDI** (or **MIDI+USB**).
   - `INPUT FROM`: Set to **USB** (or **MIDI+USB**).
   - `CLOCK RECEIVE`: Enable to sync tempo with Digitakt.
   - `TRANSPORT RECEIVE`: Enable to follow Digitakt Play/Stop commands.

4. **MIDI Channels**:
   - In `[SETTINGS] > MIDI CONFIG > CHANNELS`, ensure your Digitakt MIDI tracks output to the corresponding MIDI channels configured on your Digitone tracks.

---

## Building from Source

Requirements: macOS with Apple Command Line Tools (`clang`). Zero external libraries or package managers required.

```bash
git clone https://github.com/Steven-Robinson/midi-router.git
cd midi-router
make
make install   # Installs binary to ~/.local/bin/midi-router
```
