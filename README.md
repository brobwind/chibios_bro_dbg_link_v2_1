### Try to make a ST-LINK/V2-1 compatible bootloader based on ChibiOS/RT


#### NOTE

If you want to load ST-LINK/V2-1 firmware, you should also modify the compiled bootloader firmware.

At the offset of 0x100, the values must be 0x15 0x3c 0xa5 0x47, like this:

00000100  15 3c a5 47 31 11 00 08  31 11 00 08 31 11 00 08  |.<.G1...1...1...|
