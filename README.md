# HDHomeRun Config QUI

**Version 0.1**

HDHomeRun Config QUI is a graphical (Qt) version of the HDHomeRun TUI, for looking at and capturing TV signals from SiliconDust HDHomeRun tuners, with particular attention to ATSC 3.0. It was written with the help of Google Gemini and Anthropic Claude.

It runs on Linux, and should build on other systems that have Qt 6 and libhdhomerun.

## Features

**Tuners**
- Finds HDHomeRun devices on the network automatically, and can add a device by IP address for ones that discovery can't reach, such as a device on another subnet.
- Addresses added by IP are remembered and re-added after every scan. Right-click one in the device list to forget it.
- Uses the channel maps each device supports. Channel stepping and seek follow the selected map (broadcast stops at channel 36).

**Signal**
- Signal strength, signal quality and symbol quality, with dBm and dB values on models that report them.
- Network rate and target, TSID (ATSC 1.0), and for ATSC 3.0 the L1D BSID and SLT BSID side by side, highlighted if they don't match.
- The program list, with encrypted programs in red.
- ATSC 3.0 PLPs, in green when locked and red when not. Tick PLPs to choose which ones the tuner decodes.
- The L1 details: decoded L1-Basic and L1-Detail signaling, with PLP bitrates and required SNR, using the same parser as the TUI and RENDER. You can copy the L1 string or the PLP info to the clipboard.

**Viewing and captures**
- **View** plays the selected program in VLC.
- **Save** captures for the number of seconds you choose:
  - ATSC 1.0: a transport stream (`.ts`). The result reports transport, network and sequence errors.
  - ATSC 3.0: a debug capture (`.dbg`), or ALP or IPv4 PCAP (`.pcap`) on models that support them. A `.txt` file with the same name holds the PLP info, IDs, L1 details and raw L1 string, the same file the TUI saves.
- **Restart on Error** discards a capture as soon as an error is detected and starts again, until a clean capture finishes or you press Stop.
- Files are named as in the TUI, e.g. `rf33-bsid1234-p0p1-20261114-153000.pcap` or `rf33-tsid1234-20261114-153000.ts`, and saved to your Downloads folder unless you choose another with **Folder**.

## Keyboard shortcuts

| Key | Action |
|---|---|
| S | Scan for devices |
| V | View in VLC |
| Space | Stop (viewing, a capture, or a seek) |
| Ctrl+Left / Ctrl+Right | Seek down / up to the next channel with a signal |
| Ctrl+Up / Ctrl+Down | Next / previous channel |

## Building

You need CMake 3.16 or newer, a C/C++ compiler, Qt 6 (Widgets and Network), and libhdhomerun. VLC is needed only for **View**.

On Ubuntu or Debian:

```bash
sudo apt install build-essential cmake qt6-base-dev vlc
```

libhdhomerun is SiliconDust's library and isn't included here. Either install your distribution's package:

```bash
sudo apt install libhdhomerun-dev
```

or clone it into the project folder, where it is built along with the program (this takes priority over the package if both are present):

```bash
git clone https://github.com/Silicondust/libhdhomerun.git
```

Then build and run:

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
./build/HDHomeRun-QUI
```

## Settings

The save folder and remembered IP addresses are stored with Qt's settings, in `~/.config/HDHomeRunQUI/Config.conf` on Linux.

## Credits

- Mark J. Colombo of RabbitEars.info
- [libhdhomerun](https://github.com/Silicondust/libhdhomerun) by SiliconDust (LGPL 2.1)
- The L1 parser and PLP bitrate calculation follow `l1dump.c` from [dtv-utils](https://github.com/drmpeg/dtv-utils) by Ron Economos, and `atsc3_tables.h` is copied from its `l1dump.h` (GPLv3)
- Written with the help of Google Gemini and Anthropic Claude

`l1_detail_parser.c/.h`, `l1_details.c/.h` and `atsc3_tables.h` are shared with the HDHomeRun TUI and RENDER and are used here unchanged.

## License

GNU General Public License v3.0 or later. See [LICENSE](LICENSE).
