obs-bmusb
=========
`obs-bmusb` is a Linux plugin for [OBS studio][obs] that provides a Source for capturing from the BlackMagic USB3 cards
Intensity Shuttle and UltraStudio SDI via the [bmusb][bmusb] driver.

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

Install to the user with:
```bash
mkdir -p $HOME/.config/obs-studio/plugins/obs-bmusb/bin/64bit/
cp obs-bmusb.so $HOME/.config/obs-studio/plugins/obs-bmusb/bin/64bit/
```
or to the system with:
```bash
sudo make install
```

## Requirements

- [OBS Studio][obs] (installed via package manager)
- [bmusb][bmusb] >= 0.7.6

## Usage

After installation and restarting OBS, add the "Blackmagic USB3 source (bmusb)" source. Configure:

- **Card Index**: Which bmusb card to use (0-based)


[obs]: https://obsproject.com/
[bmusb]: https://git.sesse.net/?p=bmusb;a=summary
