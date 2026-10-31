# LLM disclosure

The contents of this repo (excepting this section) were authored entirely by Claude Fable 5.1 medium.

Barry Rountree (`barry.rountree@gmail.com`) is the responsible party.

# notepad-route

Select which channels a **Soundcraft Notepad** mixer sends to your Mac over USB, on
Macs that Soundcraft's own control panel no longer supports.

The Notepad-5, Notepad-8FX and Notepad-12FX have a software-selectable USB capture
source. Out of the box they send the **master mix**, which means every input (and the
computer's own playback, returning on the last stereo channel) arrives in your DAW as
one undifferentiated stereo pair. Changing that requires Soundcraft's *USB Audio
Control Panel*, which currently needs macOS 14 or later. Intel Macs stuck on Ventura or
earlier are out of luck.

`notepad-route` is the missing piece: a ~250-line C program with no dependencies beyond
macOS's own frameworks, plus a launchd agent that re-applies your chosen routing every
time the mixer appears on the bus (the mixer forgets the setting whenever its power
supply is removed).

## Install

Requires the Xcode Command Line Tools (`xcode-select --install`). Nothing else.

```sh
git clone https://github.com/<you>/notepad-route.git
cd notepad-route
./install.sh          # builds, installs /usr/local/bin/notepad-route (asks for sudo), loads the agent
notepad-route 0       # pick a routing once; the agent remembers it
```

`./install.sh uninstall` removes the agent and the binary.

## Usage

```
notepad-route <0..3>     set routing now and remember it
notepad-route --status   show the remembered routing
notepad-route --watch    what the launchd agent runs: re-apply on every (re)appearance
```

| index | Notepad-5        | Notepad-8FX | Notepad-12FX (capture 3+4) |
|-------|------------------|-------------|----------------------------|
| 0     | MIC 1 + LINE 2   | MIC 1+2     | MIC 3+4                    |
| 1     | LINE 2/3         | LINE 3/4    | LINE 5/6                   |
| 2     | LINE 4/5         | LINE 5/6    | LINE 7/8                   |
| 3     | MASTER L+R       | MASTER L+R  | MASTER L+R                 |

Index 3 is the power-on default. On the 12FX, capture channels 1+2 are always MIC 1+2.
The program prints 8FX labels; the indices are the same on all three models.

The agent logs to `~/Library/Logs/notepad-route.log`; the remembered routing is a single
digit in `~/Library/Application Support/notepad-route/routing`.

## Example: two instruments, two tracks, no feedback loop

Notepad-8FX, Logic Pro, a mono synth and a drum machine:

1. Synth into channel 1, drum machine into channel 2. `notepad-route 0`.
2. Logic: audio device = Soundcraft for both input and output. Track A input = Input 1,
   track B input = Input 2, software monitoring on.
3. Logic's output returns on mixer channel 7/8; that channel's fader is "how loud is the
   computer." Pull channels 1 and 2's level faders down so the dry signal doesn't reach the
   master alongside Logic's processed copy. The USB send is pre-fader, so Logic still
   hears them.

With the factory routing (3), step 3 is impossible: Logic's output comes back in on 7/8,
joins the master, and goes right back out to Logic, a few milliseconds late. It sounds
like a flanger.

## How it works

The control panel's protocol was reverse-engineered by the
[socratools](https://github.com/socratools) project for Linux
([socradoc](https://github.com/socratools/socradoc) documents it,
[socranop](https://github.com/socratools/socranop) implements it). Setting the routing
is a single 8-byte vendor control request to endpoint 0:

```
bmRequestType 0x40  bRequest 16  wValue 0  wIndex 0
data: 00 00 04 00 II 00 00 00        II = source index 0..3
```

This program sends that packet through IOKit's `IOUSBDeviceInterface::DeviceRequest`.
Because the request targets the device rather than an interface, it works while CoreAudio
has the audio interfaces open, so you can switch routing with your DAW running.

Watch mode registers an `IOServiceAddMatchingNotification` for `IOUSBHostDevice`
first-match events, filters by Harman's vendor ID (`0x05fc`), and sends the remembered
routing when the mixer appears. launchd keeps the watcher alive and starts it at login.

Two macOS-specific details that cost some time, recorded here for the next person:

- Since macOS 10.12, USB devices register as `IOUSBHostDevice`. Matching on the legacy
  `IOUSBDevice` class name finds nothing, even though `ioreg` shows
  `IOClassNameOverride = IOUSBDevice`.
- A matching dictionary containing only `idVendor` is not a legal USB match pattern and
  silently matches nothing. Enumerate all USB devices and compare the property yourself,
  which is also what libusb does.

## Observed device behavior (Notepad-8FX, firmware 1.09)

- Routing resets to MASTER L+R when the 18V supply is removed. Unplugging USB alone does
  not reset it.
- A routing packet with an out-of-place index byte is silently ignored rather than
  misinterpreted.
- Logic's input list does not change when routing changes; the USB device always
  presents two inputs. Only what feeds them changes.

## Compatibility

Tested on a 2017 iMac, macOS Ventura 13.7.8, Notepad-8FX firmware 1.09. Should build
and run on any macOS from 10.12 on, Intel or Apple silicon. Reports from Notepad-5 and
Notepad-12FX owners welcome.

## Not implemented

The mixer's ducker (threshold, range, release, meter) is documented in socradoc and
would be a small addition. Nothing here changes the audio path or firmware.

## License

MIT. Protocol documentation courtesy of the socratools authors, also MIT.
