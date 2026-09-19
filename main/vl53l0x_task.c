/*
 * VL53L0X native driver untuk proyek PEKAEM / SocaSob
 *
 * Kebutuhan versi:
 * - ESP-IDF v6.0.1 atau lebih baru
 * - Wajib memakai: driver/i2c_master.h
 *
 * JANGAN gunakan dalam proyek ini:
 * - driver/i2c.h              -> driver I2C legacy / EOL
 * - Wire.h                    -> memakai driver I2C legacy Arduino
 * - Adafruit_VL53L0X.h        -> memakai Wire.h
 *
 * Pencegahan crash:
 * - Jangan mengaktifkan CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK.
 * - Jangan menyertakan component Adafruit_VL53L0X di CMake.
 * - Sensor memakai I2C port 0.
 * - Kamera memakai SCCB/I2C port 1, sehingga tidak bentrok dengan sensor.
 *
 * Koneksi berdasarkan pins.h:
 * - SDA   : GPIO 15
 * - SCL   : GPIO 13
 * - XSHUT : GPIO 14
 * - VCC   : 3.3 V
 * - GND   : GND
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "pins.h"
#include "wifiStreamTask.h"
#include "vl53l0x_task.h"

#define VL53L0X_I2C_PORT           I2C_PORT_NUM_VL
#define VL53L0X_I2C_ADDR           0x29
#define VL53L0X_I2C_FREQUENCY_HZ   100000

#define VL53L0X_I2C_TIMEOUT_MS     100
#define VL53L0X_BOOT_DELAY_MS      100
#define VL53L0X_SAMPLE_PERIOD_MS   50

#define VL53L0X_MIN_DISTANCE_MM    30
#define VL53L0X_MAX_DISTANCE_MM    2000

static const char *TAG = "VL53L0X";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_vl53_device = NULL;
static uint8_t s_stop_variable = 0;

#define VL53_CHECK(x) do {                  \
    esp_err_t err = (x);                     \
    if (err != ESP_OK) return err;           \
} while (0)

// ============================================================================
// I2C MASTER DRIVER BARU ESP-IDF 6
// ============================================================================

static esp_err_t vl53_i2c_init(void)
{
    if (s_vl53_device != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_config = {};

    bus_config.i2c_port = VL53L0X_I2C_PORT;
    bus_config.sda_io_num = I2C_SDA_PIN;
    bus_config.scl_io_num = I2C_SCL_PIN;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.intr_priority = 0;
    bus_config.trans_queue_depth = 0;

    /*
     * Pull-up internal hanya cadangan.
     * Modul VL53L0X sebaiknya tetap memiliki pull-up eksternal pada SDA/SCL.
     */
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal membuat I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t device_config = {};

    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = VL53L0X_I2C_ADDR;
    device_config.scl_speed_hz = VL53L0X_I2C_FREQUENCY_HZ;
    device_config.scl_wait_us = 0;

    err = i2c_master_bus_add_device(
        s_i2c_bus,
        &device_config,
        &s_vl53_device
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal menambahkan VL53L0X: %s",
                 esp_err_to_name(err));
    }

    return err;
}

static esp_err_t vl53_write_u8(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};

    return i2c_master_transmit(
        s_vl53_device,
        data,
        sizeof(data),
        VL53L0X_I2C_TIMEOUT_MS
    );
}

static esp_err_t vl53_read(
    uint8_t reg,
    uint8_t *data,
    size_t length
)
{
    return i2c_master_transmit_receive(
        s_vl53_device,
        &reg,
        1,
        data,
        length,
        VL53L0X_I2C_TIMEOUT_MS
    );
}

static esp_err_t vl53_read_u8(uint8_t reg, uint8_t *value)
{
    return vl53_read(reg, value, 1);
}

static esp_err_t vl53_read_u16(uint8_t reg, uint16_t *value)
{
    uint8_t data[2];

    VL53_CHECK(vl53_read(reg, data, 2));

    *value = ((uint16_t)data[0] << 8) | data[1];

    return ESP_OK;
}

static esp_err_t vl53_write_multi(
    uint8_t reg,
    const uint8_t *data,
    size_t length
)
{
    uint8_t buffer[16];

    if (length > 15) {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[0] = reg;

    for (size_t i = 0; i < length; i++) {
        buffer[i + 1] = data[i];
    }

    return i2c_master_transmit(
        s_vl53_device,
        buffer,
        length + 1,
        VL53L0X_I2C_TIMEOUT_MS
    );
}

// ============================================================================
// HARDWARE RESET DAN DIAGNOSTIK
// ============================================================================

static void vl53_hardware_reset(void)
{
    gpio_config_t config = {};

    config.pin_bit_mask = (1ULL << VL53L0X_XSHUT_PIN);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    ESP_ERROR_CHECK(gpio_config(&config));

    gpio_set_level(VL53L0X_XSHUT_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(VL53L0X_BOOT_DELAY_MS));

    gpio_set_level(VL53L0X_XSHUT_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(VL53L0X_BOOT_DELAY_MS));
}

//static esp_err_t vl53_probe(void)
//{
    //esp_err_t err = i2c_master_probe(
        //s_i2c_bus,
        //VL53L0X_I2C_ADDR,
        //VL53L0X_I2C_TIMEOUT_MS
    //);
//
    //if (err == ESP_OK) {
        //ESP_LOGI(TAG, "VL53L0X ditemukan pada alamat 0x%02X",
                 //VL53L0X_I2C_ADDR);
    //} else {
        //ESP_LOGE(TAG, "VL53L0X tidak terdeteksi: %s",
                 //esp_err_to_name(err));
    //}
//
    //return err;
//}

//vl53 ini nanti dihapus klo udah dapet alamat
static esp_err_t vl53_probe(void)
{
    int found = 0;

    ESP_LOGI(TAG, "Memulai I2C scan...");

    for (uint8_t address = 1; address < 127; address++) {
        esp_err_t err = i2c_master_probe(
            s_i2c_bus,
            address,
            VL53L0X_I2C_TIMEOUT_MS
        );

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "I2C device ditemukan di 0x%02X", address);
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGE(TAG, "Tidak ada perangkat I2C terdeteksi.");
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

// ============================================================================
// INISIALISASI INTERNAL VL53L0X
// ============================================================================

static esp_err_t vl53_wait_interrupt(uint32_t timeout_ms)
{
    uint8_t status = 0;

    int64_t deadline = esp_timer_get_time() +
                       ((int64_t)timeout_ms * 1000);

    do {
        VL53_CHECK(vl53_read_u8(0x13, &status));

        if ((status & 0x07) != 0) {
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(2));

    } while (esp_timer_get_time() < deadline);

    return ESP_ERR_TIMEOUT;
}

static esp_err_t vl53_get_spad_info(
    uint8_t *count,
    uint8_t *type_is_aperture
)
{
    uint8_t temp = 0;

    /*
     * Tahap ini bisa lebih lama dibanding pembacaan register biasa.
     * Jangan memakai timeout 50 ms.
     */
    int64_t deadline = esp_timer_get_time() + 500000;

    VL53_CHECK(vl53_write_u8(0x80, 0x01));
    VL53_CHECK(vl53_write_u8(0xFF, 0x01));
    VL53_CHECK(vl53_write_u8(0x00, 0x00));

    VL53_CHECK(vl53_write_u8(0xFF, 0x06));

    /*
     * Bit sebelumnya harus dipertahankan.
     * Jangan langsung menulis 0x04 karena nilai register bisa berbeda
     * pada tiap unit VL53L0X.
     */
    VL53_CHECK(vl53_read_u8(0x83, &temp));
    VL53_CHECK(vl53_write_u8(0x83, temp | 0x04));

    VL53_CHECK(vl53_write_u8(0xFF, 0x07));
    VL53_CHECK(vl53_write_u8(0x81, 0x01));

    VL53_CHECK(vl53_write_u8(0x80, 0x01));
    VL53_CHECK(vl53_write_u8(0x94, 0x6B));
    VL53_CHECK(vl53_write_u8(0x83, 0x00));

    do {
        VL53_CHECK(vl53_read_u8(0x83, &temp));

        if (temp != 0x00) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(2));

    } while (esp_timer_get_time() < deadline);

    if (temp == 0x00) {
        ESP_LOGE(TAG, "Timeout saat membaca konfigurasi SPAD.");
        return ESP_ERR_TIMEOUT;
    }

    VL53_CHECK(vl53_write_u8(0x83, 0x01));
    VL53_CHECK(vl53_read_u8(0x92, &temp));

    *count = temp & 0x7F;
    *type_is_aperture = (temp >> 7) & 0x01;

    VL53_CHECK(vl53_write_u8(0x81, 0x00));
    VL53_CHECK(vl53_write_u8(0xFF, 0x06));

    /* Kembalikan hanya bit 0x04 tanpa merusak bit lain. */
    VL53_CHECK(vl53_read_u8(0x83, &temp));
    VL53_CHECK(vl53_write_u8(0x83, temp & ~0x04));

    VL53_CHECK(vl53_write_u8(0xFF, 0x01));
    VL53_CHECK(vl53_write_u8(0x00, 0x01));
    VL53_CHECK(vl53_write_u8(0xFF, 0x00));
    VL53_CHECK(vl53_write_u8(0x80, 0x00));

    ESP_LOGI(TAG, "SPAD siap: count=%u, aperture=%u",
             *count, *type_is_aperture);

    return ESP_OK;
}

static esp_err_t vl53_load_tuning_settings(void)
{
    static const uint8_t tuning[][2] = {
        {0xFF, 0x01}, {0x00, 0x00},
        {0xFF, 0x00}, {0x09, 0x00}, {0x10, 0x00}, {0x11, 0x00},
        {0x24, 0x01}, {0x25, 0xFF}, {0x75, 0x00},
        {0xFF, 0x01}, {0x4E, 0x2C}, {0x48, 0x00}, {0x30, 0x20},
        {0xFF, 0x00}, {0x30, 0x09}, {0x54, 0x00}, {0x31, 0x04},
        {0x32, 0x03}, {0x40, 0x83}, {0x46, 0x25}, {0x60, 0x00},
        {0x27, 0x00}, {0x50, 0x06}, {0x51, 0x00}, {0x52, 0x96},
        {0x56, 0x08}, {0x57, 0x30}, {0x61, 0x00}, {0x62, 0x00},
        {0x64, 0x00}, {0x65, 0x00}, {0x66, 0xA0},
        {0xFF, 0x01}, {0x22, 0x32}, {0x47, 0x14}, {0x49, 0xFF},
        {0x4A, 0x00},
        {0xFF, 0x00}, {0x7A, 0x0A}, {0x7B, 0x00}, {0x78, 0x21},
        {0xFF, 0x01}, {0x23, 0x34}, {0x42, 0x00}, {0x44, 0xFF},
        {0x45, 0x26}, {0x46, 0x05}, {0x40, 0x40}, {0x0E, 0x06},
        {0x20, 0x1A}, {0x43, 0x40},
        {0xFF, 0x00}, {0x34, 0x03}, {0x35, 0x44},
        {0xFF, 0x01}, {0x31, 0x04}, {0x4B, 0x09}, {0x4C, 0x05},
        {0x4D, 0x04},
        {0xFF, 0x00}, {0x44, 0x00}, {0x45, 0x20}, {0x47, 0x08},
        {0x48, 0x28}, {0x67, 0x00}, {0x70, 0x04}, {0x71, 0x01},
        {0x72, 0xFE}, {0x76, 0x00}, {0x77, 0x00},
        {0xFF, 0x01}, {0x0D, 0x01},
        {0xFF, 0x00}, {0x80, 0x01}, {0x01, 0xF8},
        {0xFF, 0x01}, {0x8E, 0x01},
        {0x00, 0x01}, {0xFF, 0x00}, {0x80, 0x00}
    };

    for (size_t i = 0; i < sizeof(tuning) / sizeof(tuning[0]); i++) {
        VL53_CHECK(vl53_write_u8(tuning[i][0], tuning[i][1]));
    }

    return ESP_OK;
}

static esp_err_t vl53_single_ref_calibration(uint8_t init_byte)
{
    VL53_CHECK(vl53_write_u8(0x00, 0x01 | init_byte));
    VL53_CHECK(vl53_wait_interrupt(100));

    VL53_CHECK(vl53_write_u8(0x0B, 0x01));
    VL53_CHECK(vl53_write_u8(0x00, 0x00));

    return ESP_OK;
}

static esp_err_t vl53_sensor_init(void)
{
    uint8_t model_id;
    uint8_t spad_count;
    uint8_t spad_type;
    uint8_t ref_spad_map[6];

    VL53_CHECK(vl53_read_u8(0xC0, &model_id));

    if (model_id != 0xEE) {
        ESP_LOGE(TAG, "Model ID salah: 0x%02X", model_id);
        return ESP_ERR_NOT_FOUND;
    }

    VL53_CHECK(vl53_write_u8(0x88, 0x00));

    VL53_CHECK(vl53_write_u8(0x80, 0x01));
    VL53_CHECK(vl53_write_u8(0xFF, 0x01));
    VL53_CHECK(vl53_write_u8(0x00, 0x00));

    VL53_CHECK(vl53_read_u8(0x91, &s_stop_variable));

    VL53_CHECK(vl53_write_u8(0x00, 0x01));
    VL53_CHECK(vl53_write_u8(0xFF, 0x00));
    VL53_CHECK(vl53_write_u8(0x80, 0x00));

    /* System sequence harus aktif sebelum membaca konfigurasi SPAD. */
    VL53_CHECK(vl53_write_u8(0x01, 0xFF));

    ESP_LOGI(TAG, "Membaca konfigurasi SPAD...");
    VL53_CHECK(vl53_get_spad_info(&spad_count, &spad_type));

    VL53_CHECK(vl53_write_u8(0xFF, 0x01));
    VL53_CHECK(vl53_write_u8(0x4F, 0x00));
    VL53_CHECK(vl53_write_u8(0x4E, 0x2C));
    VL53_CHECK(vl53_write_u8(0xFF, 0x00));

    /* Register referensi SPAD yang wajib diatur sebelum menulis SPAD map. */
    VL53_CHECK(vl53_write_u8(0xB4, 0xB4));

    VL53_CHECK(vl53_write_multi(0xB0, ref_spad_map, 6));

    int first_spad = spad_type ? 12 : 0;
    int enabled = 0;

    for (int i = 0; i < 48; i++) {
        if (i < first_spad || enabled == spad_count) {
            ref_spad_map[i / 8] &= ~(1 << (i % 8));
        } else if (ref_spad_map[i / 8] & (1 << (i % 8))) {
            enabled++;
        }
    }

    VL53_CHECK(vl53_write_u8(0xFF, 0x01));
    VL53_CHECK(vl53_write_u8(0x4F, 0x00));
    VL53_CHECK(vl53_write_u8(0x4E, 0x2C));
    VL53_CHECK(vl53_write_u8(0xFF, 0x00));
    VL53_CHECK(vl53_write_multi(0xB0, ref_spad_map, 6));

    VL53_CHECK(vl53_load_tuning_settings());

    {
        uint8_t gpio_hv_mux_active_high;

        VL53_CHECK(vl53_write_u8(0x0A, 0x04));
        VL53_CHECK(vl53_read_u8(0x84, &gpio_hv_mux_active_high));
        VL53_CHECK(vl53_write_u8(
            0x84,
            gpio_hv_mux_active_high & ~0x10
        ));
        VL53_CHECK(vl53_write_u8(0x0B, 0x01));
    }

    VL53_CHECK(vl53_write_u8(0x01, 0xE8));

    /* VHV calibration */
    VL53_CHECK(vl53_write_u8(0x01, 0x01));
    VL53_CHECK(vl53_single_ref_calibration(0x40));

    /* Phase calibration */
    VL53_CHECK(vl53_write_u8(0x01, 0x02));
    VL53_CHECK(vl53_single_ref_calibration(0x00));

    /* Kembalikan konfigurasi normal */
    VL53_CHECK(vl53_write_u8(0x01, 0xE8));

    return ESP_OK;
}

// ============================================================================
// MODE PENGUKURAN CONTINUOUS
// ============================================================================

static esp_err_t vl53_start_continuous(void)
{
    VL53_CHECK(vl53_write_u8(0x80, 0x01));
    VL53_CHECK(vl53_write_u8(0xFF, 0x01));
    VL53_CHECK(vl53_write_u8(0x00, 0x00));
    VL53_CHECK(vl53_write_u8(0x91, s_stop_variable));
    VL53_CHECK(vl53_write_u8(0x00, 0x01));
    VL53_CHECK(vl53_write_u8(0xFF, 0x00));
    VL53_CHECK(vl53_write_u8(0x80, 0x00));

    // Continuous back-to-back ranging
    return vl53_write_u8(0x00, 0x02);
}

static esp_err_t vl53_read_continuous(uint16_t *distance_mm)
{
    VL53_CHECK(vl53_wait_interrupt(100));
    VL53_CHECK(vl53_read_u16(0x1E, distance_mm));

    // Bersihkan interrupt agar pengukuran berikutnya berjalan
    VL53_CHECK(vl53_write_u8(0x0B, 0x01));

    return ESP_OK;
}

// ============================================================================
// TASK YANG DIJALANKAN OLEH app_main()
// ============================================================================

void vTaskVL53L0X(void *pvParameters)
{
    vl53_hardware_reset();

    esp_err_t err = vl53_i2c_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init gagal: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = vl53_probe();

    if (err != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }

    err = vl53_sensor_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Inisialisasi sensor gagal: %s",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = vl53_start_continuous();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Continuous mode gagal: %s",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "VL53L0X native ESP-IDF siap.");

    while (true) {
        uint16_t distance_mm = 0;

        err = vl53_read_continuous(&distance_mm);

        bool valid = (
            err == ESP_OK &&
            distance_mm >= VL53L0X_MIN_DISTANCE_MM &&
            distance_mm <= VL53L0X_MAX_DISTANCE_MM
        );

        if (valid) {
            ESP_LOGD(TAG, "Jarak: %u mm", distance_mm);
            ml_stream_set_distance(distance_mm, true);
        } else {
            ml_stream_set_distance(0, false);

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Pembacaan gagal: %s",
                         esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(VL53L0X_SAMPLE_PERIOD_MS));
    }
}