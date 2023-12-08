#ifndef MAIN_AD_H_
#define MAIN_AD_H_

#include <stdint.h>
#include "freertos/FreeRTOS.h"

#define MAX_CHANNELS 2  // Define the number of channels

#define AD_MAX (4096)
#define AD_MIN (0)

// Initialize the ADC
BaseType_t ad_init();

// Get the raw ADC value for a specified channel
BaseType_t ad_get(float *value, TickType_t wait, int channel);

// Get the calculated temperature for a specified channel
BaseType_t ad_get_temperature(float *value, TickType_t ticks, int channel);

// Deinitialize the ADC
BaseType_t ad_deinit();

#endif /* MAIN_AD_H_ */
