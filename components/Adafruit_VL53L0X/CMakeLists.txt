idf_component_register(
    SRCS
        "src/Adafruit_VL53L0X.cpp"
        "src/core/src/vl53l0x_api.cpp"
        "src/core/src/vl53l0x_api_calibration.cpp"
        "src/core/src/vl53l0x_api_core.cpp"
        "src/core/src/vl53l0x_api_ranging.cpp"
        "src/core/src/vl53l0x_api_strings.cpp"
        "src/platform/src/vl53l0x_i2c_comms.cpp"
        "src/platform/src/vl53l0x_platform.cpp"
    INCLUDE_DIRS
        "src"
    REQUIRES
        "arduino-esp32"
)

# Kode vendor Adafruit/ST ini ditulis sebelum GCC sekencang ini ada, dan
# memicu warning baru (maybe-uninitialized di vl53l0x_api.cpp, volatile++
# deprecated di C++26 pada vl53l0x_platform.cpp) yang oleh project ini
# diperlakukan sebagai error lewat -Werror. Matikan -Werror HANYA untuk
# 2 warning ini, HANYA untuk component ini -- tidak menyentuh pengaturan
# -Wall -Werror project secara global.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=maybe-uninitialized
    -Wno-error=volatile
)