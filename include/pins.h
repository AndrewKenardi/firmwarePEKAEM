#ifndef CAMERA_PINS_H_
#define CAMERA_PINS_H_

#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1 // Terhubung ke RST board / tidak pakai GPIO khusus
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27

#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    33
#define CAM_PIN_PCLK    22

#define I2C_PORT_NUM_VL      I2C_NUM_0  
#define I2C_SDA_PIN       GPIO_NUM_15
#define I2C_SCL_PIN       GPIO_NUM_13

#define VL53L0X_XSHUT_PIN GPIO_NUM_4

#define VL53L0X_CALIBRATION_OFFSET_UM 15000
#define VL53L0X_CALIBRATION_XTALK_MCPS 0.0
#define VL53L0X_CALIBRATION_REF_SPAD \
    &(vl53l0x_ref_spad_calibration_t){ \
        .count = 3, \
        .is_aperture = true \
    }

#define INTER_MEASUREMENT_MS 100


#endif