## Model Comparison

| Product                        | Module                   | QWIIC | Seamless <br> power <br> switching | GNSS<br> routing<br> to SOC | GNSS<br>PPS | eSIM<br>Pad | BMS | Camera<br>Interface | DeepSleep<br> Current | Pin<br>compatible |
| ------------------------------ | ------------------------ | ----- | ---------------------------------- | --------------------------- | ----------- | ----------- | --- | ------------------- | --------------------- | ----------------- |
| T-A7670X                       | ESP32-WROVER-B(N4R8)     | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | Avg: 157uA            | ❌                 |
| T-A7608X                       | ESP32-WROVER-B(N4R8)     | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | Avg: 240uA            | ❌                 |
| T-SIM7000G                     | ESP32-WROVER-B(N4R8)     | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | Avg: 500uA            | ❌                 |
| T-SIM7600G                     | ESP32-WROVER-B(N4R8)     | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | Avg: 200uA            | ❌                 |
| T-SIM7080G-S3                  | ESP32-S3-WROOM-1(N16R8)  | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ❌   | ✅                   | Avg: 1.1mA            | ❌                 |
| T-SIM7670G-S3                  | ESP32-S3-WROOM-1(N16R8)  | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | Avg: 497uA            | ❌                 |
| T-A7608X-S3                    | ESP32-S3-WROOM-1(N16R8)  | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | Avg: 368uA            | ❌                 |
| T-A7670X-S3<br>-**Standard**   | ESP32-S3-WROOM-1(NR16R2) | ✅     | ✅                                  | ✅                           | ✅           | ✅           | ✅   | ✅                   | Avg: 314uA            | ✅                 |
| T-SIM7670G-S3<br>-**Standard** | ESP32-S3-WROOM-1(NR16R2) | ✅     | ✅                                  | ✅                           | ✅           | ✅           | ✅   | ✅                   | Avg: 147uA            | ✅                 |
| T-SIM7000G-S3<br>-**Standard** | ESP32-S3-WROOM-1(NR16R2) | ✅     | ✅                                  | ✅                           | ❌           | ✅           | ✅   | ✅                   | Avg: 166uA            | ✅                 |
| T-SIM7080G-S3<br>-**Standard** | ESP32-S3-WROOM-1(NR16R2) | ✅     | ✅                                  | ✅                           | ❌           | ✅           | ✅   | ✅                   | Avg: 128uA            | ✅                 |
| T-SIM7600G-S3<br>-**Standard** | ESP32-S3-WROOM-1(NR16R2) | ✅     | ✅                                  | ❌                           | ❌           | ✅           | ✅   | ✅                   | Avg: 128uA            | ✅                 |

- ESP32-WROVER-B(N4R8): 4MB Flash , 8MB PSRAM
- ESP32-S3-WROOM-1(N16R8): 16MB Flash , 8MB PSRAM(Octal SPI)
- ESP32-S3-WROOM-1(N16R2): 16MB Flash , 2MB PSRAM(Quad SPI)
- The deep sleep current test record can be found in [DeepSleep.ino](../examples/DeepSleep/DeepSleep.ino)
- SIM7000G/SIM7080G/SIM7600G do not have PPS function, so this function is not available.
