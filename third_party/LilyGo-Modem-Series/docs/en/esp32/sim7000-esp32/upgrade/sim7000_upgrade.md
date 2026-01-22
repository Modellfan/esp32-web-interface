
<div align="center" markdown="1">
  <img src="../../../../../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGo SIM7000 Upgrade Guide🌟</h1>

## Resources

- [SIM7000 FlashTools v1.5.8](https://drive.google.com/file/d/1BUtRWizTqEeI8x4khVRwp_ITektyUyoN/view?usp=sharing)
- [SIM7000 Driver](https://drive.google.com/file/d/1f02TTNoyirFPGWbob1khy9dnoBonoVe7/view?usp=sharing)
- [1529B11V01SIM7000G Fixed MQTTS,HTTPS](https://drive.google.com/file/d/12rZ9b7z3ONCPwtevOcz3khYl5vw4zGL3/view?usp=sharing)
- [Tools and files for backing up and restoring QCN](https://drive.google.com/drive/folders/10Fik8zT4UFX1dmCLbZ0GkgIIsxu356QT)
- [SIM7000 FlashTools v1.4.3](https://drive.google.com/file/d/1-ADY7_fbXehiQBJZhwp7Kht-ZQpFE1np/view?usp=sharing)

> \[!IMPORTANT]
>
> Note: The recommended upgrade tool version is v1.5.8. Other versions are not recommended as they may cause loss of qcn.
>


## `1` Write to ATDebug

* Write [ATDebug](../../../../../examples/ATdebug/ATdebug.ino) to start the modem without running any application

## `2` Connect to the Modem USB port

- The SIM7000G ESP32 version has a reserved USB solder point on the battery holder side for the modem. You need to connect a USB cable to this solder point, or directly solder a USB-A cable to it, as shown below.
- The SIM7000G-S3-Standard version requires two USB-C ports.
- Connect both the ESP32 USB-C port and the modem's USB port to your computer, using one port for power and the other for modem upgrades.

![USB](./images/16.png)
![USB](./images/17.png)

## `3` Update Driver

![update_driver](./images/update_driver.gif)

## `4` Update the firmware

1. Click **Load -> MDM9206(SIM7000Series)**

![upgrade-1](./images/upgrade-1.png)

2. Click **... -> Select the firmware folder (the downloaded firmware is compressed in rar or zip format and needs to be unzipped first)**

![upgrade-2](./images/upgrade-2.png)

3. Close Firmware Selection -> Click **Start**

![upgrade-3](./images/upgrade-3.png)

4. Wait for the upgrade to complete

![upgrade-4](./images/upgrade-4.png)

5. After the upgrade is complete, the modem's USB port will appear in the device manager again. Click **stop**

![upgrade-5](./images/upgrade-5.png)

6. Open the serial monitor, restart ESP32, and send `AT+SIMCOMATI` to query the current running firmware version.

![upgrade-6](./images/upgrade-6.png)

7. The upgrade is now complete