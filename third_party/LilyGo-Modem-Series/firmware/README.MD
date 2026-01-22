<div align="center" markdown="1">
  <img src="../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGo-Modem-Firmware</h1>


## ESP32 Version

| Product Name            | AT Debug Firmware                                               |
| ----------------------- | --------------------------------------------------------------- |
| [T-A7670X-ESP32][1]     | [firmware](./debug/esp32/T-A7670X_ATdebug_251201.bin)           |
| [T-Call-A7670X-v1.0][2] | [firmware](./debug/esp32/T-Call-A7670X-V1-0_ATdebug_251201.bin) |
| [T-Call-A7670X-v1.1][2] | [firmware](./debug/esp32/T-Call-A7670X-V1-1_ATdebug_251201.bin) |
| [T-A7608][3]            | [firmware](./debug/esp32/T-A7608X_ATdebug_251201.bin)           |
| [T-SIM7000G][11]        | [firmware](./debug/esp32/T-SIM7000G_ATdebug_251201.bin)         |
| [T-SIM7070G][10]        | [firmware](./debug/esp32/T-SIM7070G_ATdebug_251201.bin)         |
| [T-SIM7600G][9]         | [firmware](./debug/esp32/T-SIM7600X_ATdebug_251201.bin)         |
| [T-PCI-E][8]            | [firmware](./debug/esp32/T-PCIE_ATdebug_251201.bin)             |

## ESP32-S3 Version

| Product Name        | AT Debug Firmware                                          |
| ------------------- | ---------------------------------------------------------- |
| [T-A7608-S3][4]     | [firmware](./debug/esp32s3/T-A7608X-S3_ATdebug_251201.bin) |
| [T-SIM7670G-S3][5]  | [firmware](./debug/esp32s3/T-SIM7670G_ATdebug_251201.bin)  |
| [T-ETH-Elite-S3][7] | [firmware](./debug/esp32s3/T-ETH-ELITE_ATdebug_251201.bin) |

## Standard-Series

| Product Name                            | AT Debug Firmware                                                                           |
| --------------------------------------- | ------------------------------------------------------------------------------------------- |
| [StandardSeries_ATdebug][6]             | [firmware](./debug/standard-series/StandardSeries_ATdebug_251201.bin)                       |
| [StandardSeries_CameraWebServer_SoftAP] | [Camera firmware](./debug/standard-series/StandardSeries_CameraWebServer_SoftAP_251201.bin) |

* Standard Series pin compatible, no need to distinguish between different modemsStandard Series

[1]: https://www.lilygo.cc/products/t-sim-a7670e
[2]: https://www.lilygo.cc/products/t-call-v1-4
[3]: https://lilygo.cc/products/t-a7608e-h?variant=42860532433077
[4]: https://lilygo.cc/products/t-a7608e-h
[5]: https://lilygo.cc/products/t-sim-7670g-s3
[6]: https://www.lilygo.cc/products
[7]: https://lilygo.cc/products/t-eth-elite-1
[8]: https://lilygo.cc/products/a-t-pcie
[9]: https://lilygo.cc/products/t-SIM7600
[10]: https://lilygo.cc/products/t-sim7070g
[11]: https://lilygo.cc/products/t-sim7000g

## Use ESP Download Tool

* [Flash Download Tool User Guide](https://docs.espressif.com/projects/esp-test-tools/en/latest/esp32/production_stage/tools/flash_download_tool.html)

| Steps  | ESP32 Version                   | ESP32-S3 Version                    |
| ------ | ------------------------------- | ----------------------------------- |
| Step 1 | ![esp32dev](images/esp32-1.png) | ![esp32s3dev](images/esp32s3-1.png) |
| Step 2 | ![esp32dev](images/esp32-2.png) | ![esp32s3dev](images/esp32s3-2.png) |

### Use Web Flasher

* [ESP Web Flasher Online](https://espressif.github.io/esptool-js/)

![web_flasher](./images/web_flasher.gif)

* Note that after writing is completed, you need to press RST to reset.

### Use command line

If system asks about install Developer Tools, do it.

```bash
python3 -m pip install --upgrade pip
python3 -m pip install esptool
```

In order to launch `esptool.py`, exec directly with this:

```bash
python3 -m esptool
```

For **ESP32-S3** use the following command to write

```bash
esptool --chip esp32s3  --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m 0x0 firmware.bin
```

For **ESP32** use the following command to write

```bash
esptool --chip esp32  --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m 0x0 firmware.bin
```

# 2️⃣FAQ

- **Can't upload any sketch，Please enter the upload mode manually.**
   1. Connect the board via the USB cable
   2. Press and hold the BOOT button , While still pressing the BOOT button (If there is no BOOT button, you need to use wires to connect GND and IO0 together.)
   3. Press RST button
   4. Release the RST button
   5. Release the BOOT button (If there is no BOOT button, disconnect IO0 from GND.)
   6. Upload sketch

> \[!IMPORTANT]
> ESP32 version Since the SD card uses IO2 as CS, you cannot upload software in the Arduino IDE when you insert the SD card, so you must remove the SD card when uploading new code
>
