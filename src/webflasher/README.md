# webflasher

Browser-based flasher for **wyze-hijack-ap** (ESP32-S3) to make it easy for users to get code execution on their bulbs.

1. **Connect** – pick the board's serial port; the chip is detected (expects ESP32-S3)
2. **Flash** – writes the three images at their offsets
3. **Monitor** – reconnects at 115200 and streams output; Reset re-runs the app.


## Refresh bundled firmware

build.py will build `wyze-loader` and `wyze-hijack-ap` and place the final binary inside of the webflasher directory. There is also configuration for GitHub pages.
