# LED Control

A 3DS homebrew app to control the notification LED color and behavior.

## Compatibility

Works on all 3DS models running firmware 9.0+:
- Old 3DS / Old 3DS XL
- New 3DS / New 3DS XL
- 2DS / New 2DS XL

## Installation

1. Download `led_control.3dsx` from the [releases page](../../releases)
2. Copy it to `sdmc:/3ds/led_control/led_control.3dsx` on your SD card
3. Launch via the Homebrew Launcher

## Building from Source

**Requirements:**
- [devkitPro](https://devkitpro.org) with devkitARM
- libctru
- citro2d / citro3d

Install dependencies via dkp-pacman:
```bash
dkp-pacman -S 3ds-dev 3ds-citro2d
```

Then build:
```bash
make
```

## License

MIT License — see [LICENSE](LICENSE)
