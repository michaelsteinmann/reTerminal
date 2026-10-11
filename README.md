# reTerminal

## SwissMicros gallery (`swissmicros_gallery.yaml`)

Battery-friendly ESPHome firmware for the Seeed reTerminal E1002 (7.3" Spectra E6).
It shows one calculator from the SwissMicros webshop per day, with a battery
indicator in the bottom-right corner.

- **Left white button:** previous image. **Right white button:** next image.
- **Green button:** WiFi setup. The screen shows instructions; join the
  `SwissMicros Gallery` network and pick your WiFi in the captive portal
  (open for 5 minutes). Starts automatically on power-on until WiFi is set up.
- The device wakes, refreshes the panel (~20 s) and deep-sleeps until the next
  day or the next button press. The current image is kept in flash.
- **Firmware updates:** on the daily wake and after power-on (once WiFi is set up) it connects for
  a few seconds and installs the latest GitHub release if its version differs.
  Builds without a version (`dev`) never auto-update. While updating, the
  screen shows "Updating firmware" and the LED blinks during the download.
- Days are counted from power-on with the ESP32's internal RTC, so the change
  happens roughly every 24 h. It is not aligned to midnight.

Regenerate the images (downloads from the webshop and dithers to the 6-colour palette):

    python tools/build_images.py --font-dir <dir with Theinhardt-Bold.otf / -Medium.otf>

Build and flash over USB (pack the committed PNGs into `gallery_images.h` first):

    python tools/build_images.py --pack-only
    esphome run swissmicros_gallery.yaml --device /dev/ttyUSB0

Release an OTA update: push a version tag, e.g. `git tag v1.1.0 && git push origin v1.1.0`.
`.github/workflows/release.yml` builds it and publishes `firmware.ota.bin`,
`firmware.factory.bin` and `manifest.json` as a GitHub release.
