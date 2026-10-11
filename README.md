# reTerminal

## SwissMicros gallery (`swissmicros_gallery.yaml`)

Battery-friendly ESPHome firmware for the Seeed reTerminal E1002 (7.3" Spectra E6).
It shows one calculator from the SwissMicros webshop per day, with a battery
indicator in the bottom-right corner.

- **Left white button:** previous image. **Right white button:** next image.
- No WiFi. The device wakes, refreshes the panel (~20 s) and deep-sleeps until
  the next day or the next button press. The current image is kept in flash.
- Days are counted from power-on with the ESP32's internal RTC, so the change
  happens roughly every 24 h. It is not aligned to midnight.

Regenerate the images (downloads from the webshop and dithers to the 6-colour palette):

    python tools/build_images.py --font-dir <dir with Theinhardt-Bold.otf / -Medium.otf>

Build and flash:

    esphome run swissmicros_gallery.yaml --device /dev/ttyUSB0
