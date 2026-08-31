# LED

I frankly didn't spend too much time on the LED behavior, I just wanted to make the bulb turn a distinct color once the loader code was running so I could tell when the OTA image handover was complete.

## Basics

The LED channels are output via I2C to what I believe is a BP5758, the I2C instructions matched the disassembly and worked