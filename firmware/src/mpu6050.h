/* Real MPU6050 over I2C (SCL=16, SDA=17). Used by the IMU debug page.
 * The racing engine still uses the simulated mpu_t in sim.c. */
#pragma once
#include <stdbool.h>

bool mpu6050_init(void);          /* create I2C bus, probe 0x68/0x69, configure */
bool mpu6050_present(void);
bool mpu6050_read(float *ax, float *ay, float *az,   /* g */
                  float *gx, float *gy, float *gz,   /* deg/s */
                  float *temp_c);
