# Ringkasan Perbaikan Integrasi Adafruit VL53L0X

Project asli sebenarnya sudah **90% siap** (pin sudah didefinisikan di `pins.h`,
`arduino-esp32` sudah jadi dependency di `idf_component.yml`, kode task sensor
sudah ditulis di `main.cpp`) — tapi ada 4 bug yang membuatnya tidak pernah bisa
di-build. Berikut yang diperbaiki:

## 1. Lokasi library salah (`main/CMakeLists.txt`)
`main/CMakeLists.txt` lama mereferensikan:
```
../components/Adafruit_VL53L0X/src/Adafruit_VL53L0X.cpp
```
Padahal folder `FirmwarePkm/components/` **kosong** — library aslinya ada di
`main/libraries/Adafruit_VL53L0X/`. CMake akan langsung gagal saat configure
karena file tidak ditemukan.

**Fix:** library dipindah ke `components/Adafruit_VL53L0X/` (lokasi standar
untuk ESP-IDF component) dan didaftarkan lewat `REQUIRES "Adafruit_VL53L0X"`,
bukan dikompilasi manual satu file saja.

## 2. Source file library tidak lengkap (`components/Adafruit_VL53L0X/src/CMakeLists.txt`)
File `platform/src/vl53l0x_platform.cpp` (berisi `VL53L0X_WrByte`,
`VL53L0X_RdByte`, `VL53L0X_PollingDelay`, dll — dipakai di seluruh core API)
tidak ada di daftar `SRCS`. Akan menyebabkan **linker error** (undefined
reference) begitu file lain berhasil di-fix.

`INCLUDE_DIRS` juga menunjuk ke `core/inc/` dan `platform/inc/` yang **tidak
pernah ada** (header aslinya rata di folder `src/`). Ini bikin CMake configure
error duluan sebelum sempat sampai ke masalah linker.

**Fix:** tambahkan `platform/src/vl53l0x_platform.cpp` ke `SRCS`, dan
`INCLUDE_DIRS` disederhanakan jadi `"."` saja.

## 3. `update_manager.c` tidak pernah dikompilasi
`main/CMakeLists.txt` tidak menyertakan `update_manager.c`, padahal
`vTaskUpdateManager` dipanggil dan dibutuhkan untuk fitur OTA. (`version_check.c`
sengaja **tidak** ditambahkan karena `version_check_fetch` sudah didefinisikan
ulang di `ota_task.c` — kalau keduanya dikompilasi akan jadi duplicate symbol.)

## 4. `main.cpp` — I2C tidak pernah diinisialisasi + fitur WiFi/OTA hilang
- `Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN)` di-comment. `Adafruit_VL53L0X::begin()`
  **tidak** memanggil `Wire.begin()` sendiri, jadi I2C akan jatuh ke pin default
  ESP32 (GPIO21/22) yang di board ini sudah dipakai kamera
  (`CAM_PIN_D3`/`CAM_PIN_PCLK`) → tabrakan pin, sensor gagal total.
- `main.cpp` (yang sekarang dikompilasi, bukan `main.c`) tidak pernah
  menjalankan `vTaskWifiConnect`, `udp_logger_init`, atau `vTaskUpdateManager`
  — jadi WiFi/OTA/logging mati diam-diam.
- Ditambahkan juga toggle pin `VL53L0X_XSHUT_PIN` sebelum `begin()` untuk
  memastikan sensor dalam kondisi reset bersih saat boot.

**Fix:** `main.cpp` digabung ulang supaya mencakup alur WiFi/OTA/UDP-logger
lengkap dari `main.c` sekaligus task VL53L0X yang sudah diperbaiki.
`main.c` dibiarkan ada sebagai referensi tapi tidak dikompilasi (tidak ada di
`main/CMakeLists.txt`).

## 5. (Update) `CMakeLists.txt` component harus di root folder, bukan di `src/`
ESP-IDF mendeteksi sebuah folder sebagai component **hanya** kalau
`CMakeLists.txt` ada persis di root folder tersebut (`components/Adafruit_VL53L0X/CMakeLists.txt`),
bukan di subfolder seperti `components/Adafruit_VL53L0X/src/CMakeLists.txt`.
Kalau tidak, akan muncul error:
```
Failed to resolve component 'Adafruit_VL53L0X' required by component 'main': unknown name.
```
Sudah diperbaiki: `CMakeLists.txt` sekarang ada di
`components/Adafruit_VL53L0X/CMakeLists.txt` dan semua path source-nya
diawali `src/` (mis. `src/Adafruit_VL53L0X.cpp`).

## Wiring yang diasumsikan (sesuai `pins.h`)
| Sinyal | GPIO |
|---|---|
| I2C SDA | 15 |
| I2C SCL | 13 |
| XSHUT   | 4  |

Jika wiring fisik Anda beda, ubah nilainya di `include/pins.h`.

## Cara build
```bash
cd FirmwarePkm
idf.py set-target esp32          # sesuaikan kalau target beda
idf.py build
idf.py -p /dev/ttyUSBx flash monitor
```
`idf.py build` akan otomatis mengunduh ulang `managed_components/`
(arduino-esp32, esp32-camera, dll — sesuai `dependencies.lock`) karena folder
tersebut sengaja tidak disertakan di paket ini supaya ukurannya kecil.
