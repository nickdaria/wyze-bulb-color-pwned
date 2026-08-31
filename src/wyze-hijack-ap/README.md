# wyze-hijack-ap (ESP32-S3)

An ESP32S3 program that I spun up because I had a LILYGO ESP32S3 display laying around. This runs seprately and the functionality is insanely simple.

- Hosts an AP with the expected credentials
- Serves an embedded OTA image at an endpoint
- Attempts to fire the upgrade command with that endpoint a few times for every new WiFi client

All you have to do is get into loader mode by spamming your light switch.

## Building

The embedded firmware image must be placed in the folder and called `payload.bin` which gets shoved into the binary by CMake. **Don't forget to copy/clean/rebuild when you make changes to the underlying image** served as it gets compiled into the binary.

## Usage

- Flash `hijack-ap` to an ESP32-S3 with an appropriate power supply
- Watch logs (if you wanna)
- Power cycle your bulb exactly 17 times, stop and wait for the wyze-loader-XXXXXX AP. It can take a minute.

I have not tested this with multiple bulbs at once.

## Notes

- I put the console on USB UART because that's what all of my dev boards have. Feel free to change.