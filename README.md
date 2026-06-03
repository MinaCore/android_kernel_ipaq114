#  Android kernel for iPAQ 11x devices 
Based on ANDROMNiA project, Android kernel for Samsung Omnia. Kernel version 2.6.32

## How to build
```bash
make ARCH=arm CROSS_COMPILE=/path/to/arm2008-q3/bin/arm-none-eabi- -j$(nproc)
```
You can download the **arm2008-q3** toolchain from the [Releases](https://github.com/MinaCore/android_kernel_ipaq114/releases/tag/toolchain) section.

## Usage

1. **Download:** Get the `build.zip` archive from the [Releases](https://github.com/MinaCore/android_kernel_ipaq114/releases/latest) section.
2. **Prepare the SD card:** 
   - Partition 1 (Primary): **ext2** (for the root filesystem).
   - Partition 2 (Primary): **FAT32** (for boot files).
3. **Setup rootfs:** Extract the contents of the `rootfs` folder from the `build.zip` archive into the **ext2** partition.
4. **Setup boot files:** Copy `haret.exe`, `default.txt`, and `Image` from the archive to the **FAT32** partition.
5. **Launch:** Run `haret.exe` from the **FAT32** partition on your device.

*Note: The partition order is critical. Ensure the ext2 partition is the first one on the SD card, otherwise the system will fail to mount the root filesystem.*

## Hardware support
Hardware support status code: C-

|Boot process <br>(all ticks for D status)||
| ------------- |:------------:|
|Boot process|✔️|

|Boot-critical hardware<br>(all ticks for C status)||
| ------------- |:------------:|
|Basic hardware|✔️|
|USB to host|❌|
|SD-MMC|✔️|
|PCMCIA(CF)|N/A|
|Display|✔️|

|Basic PDA hardware<br>(all ticks for B status)||
| ------------- |:------------:|
|Buttons|✔️|
|Touchscreen|❌|
|Suspend/Resume|❌|
|Battery status|❌|
|APM status|❌|
|Backlight|❌|
|GUI launching|✔️|

|Advanced PDA hardware<br>(all ticks for A status)||
| ------------- |:------------:|
|Sound|❌|
|Flash memory|❌|
|Serial|N/A|
|LEDs|✔️|
|Bootloader|N/A|

|Full hardware support <br>(all ticks for A+ status)||
| ------------- |:------------:|
|Bluetooth|❌|
|WiFi|❌|

## Credits

This project would not have been possible without the following contributions:

* **ANDROMNiA** - for providing the base kernel that this project is built upon.
* **nrndda** - for the work on a similar device, which provided essential insights for my driver implementation.
* **Oliver Ford** - for extensive technical documentation and research on this device.
* **My girlfriend** - for the endless moral support during the long development nights.

## Contact & Community

If you have any questions, bug reports, or just want to follow my development progress, feel free to join my Telegram channel:

**[Join my Telegram channel](https://t.me/ipaq114)**
