// wind.h

#ifndef _WIND_H_
#define _WIND_H_
#include <stdint.h>  // Add this line
#include "driver/gpio.h"

#define WIND_GPIO 17
#define WIND_FACTOR 0.1 // conversion factor to km/hr



// Function prototypes
void wind_speed_task(void *args);
BaseType_t wind_init(void);

void wind_deinit();

#endif /* !_WIND_H_ */
// wind.


