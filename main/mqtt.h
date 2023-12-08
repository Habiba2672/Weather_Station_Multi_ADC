/*
 * mqtt.h
 *
 *  Created on: Dec 3, 2023
 *  Author: alex technology
 */

#ifndef MAIN_MQTT_H_
#define MAIN_MQTT_H_

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"  // Include the FreeRTOS task header

// Define BaseType_t if it's not defined (this might not be necessary depending on your FreeRTOS version)
#ifndef BaseType_t
#define BaseType_t int
#endif

BaseType_t mqtt_init();
BaseType_t mqtt_deinit();
BaseType_t send_mqtt(const char *topic, char *msg);

int subscribe(const char *topic);
int un_subscribe(const char *topic);
int publish(const char *topic, const char *msg);
int unpublish(const char *topic, const char *msg);
void register_cmd();

#endif /* MAIN_MQTT_H_ */
