/* Battery voltage sense.
 * VBAT_EN (GPIO21) is normally LOW; pulse HIGH to connect the divider, then
 * read VBAT_ADC (GPIO1, ADC1_CH0). The pin voltage is half the battery, so the
 * real battery voltage = measured * 2.
 */
#pragma once

void battery_init(void);
void battery_update(void);   /* call periodically; samples ~1x/second */
int  battery_mv(void);       /* last measured battery voltage in mV (0 = unknown) */
