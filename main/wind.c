#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "argtable3/argtable3.h"
#include "esp_console.h"

#include "wind.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "mqtt.h"
#include "mac_address.h"
#include "esp_mac.h"
#include "time.h"
#include "ad.h"
#define WIND_GPIO 17
#define WIND_SPEED_TASK_Hz 1
#define WIND_FACTOR 0.1  // You need to set the appropriate value based on calibration
#define WIND_SPEED_MIN 0.0  // Minimum wind speed in km/h
#define WIND_SPEED_MAX 100.0  // Maximum wind speed in km/h

#define TOPIC_WIND "sensor/wind"

typedef struct {
  float speed;
  uint32_t pulses;
  uint32_t errors;
} wind_sensor_data_t;

static int register_wind_cmd();
extern wind_sensor_data_t wind;
static SemaphoreHandle_t wind_lock;

// Interrupt service routine
void IRAM_ATTR wind_isr() {
    wind.pulses++;  // just count pulses
    xSemaphoreGiveFromISR(wind_lock, NULL); // Notify from ISR

}

// Calculation function called periodically
void calculate_wind_speed() {
    if (xSemaphoreTake(wind_lock, portMAX_DELAY)) {
        if (wind.pulses > 0) {
            wind.speed = (float)wind.pulses * WIND_FACTOR;
            wind.pulses = 0;
        } else {
            wind.errors++;  // handle error
        }
        xSemaphoreGive(wind_lock);
    }
}

// FreeRTOS task to read wind speed
void wind_speed_task(void *args) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << WIND_GPIO),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&io_conf);

    // GPIO interrupt on rising edge
    gpio_install_isr_service(0);
    gpio_isr_handler_add(WIND_GPIO, wind_isr, NULL);

    while (1) {
        calculate_wind_speed();
        vTaskDelay(1000 / WIND_SPEED_TASK_Hz);
    }
}

BaseType_t wind_init(void) {
    // Initialize wind module here, if necessary
    wind_lock = xSemaphoreCreateBinary();
    xSemaphoreGive(wind_lock);
    register_wind_cmd();
    // Initialize semaphore
    return pdPASS;
}

BaseType_t get_speed(float *value, TickType_t wait) {
	if (!value)
		return pdFAIL;
	if (xSemaphoreTake(wind_lock, wait) == pdPASS) {
		*value = wind.speed;
		xSemaphoreGive(wind_lock);
		return pdPASS;
	}
	return pdFAIL;
}
extern BaseType_t send_mqtt(const char *topic, char *msg);

BaseType_t wind_send() {
    cJSON *root = cJSON_CreateObject();
    float currentSpeed;
    if (get_speed(&currentSpeed, portMAX_DELAY) == pdPASS) {
        cJSON_AddNumberToObject(root, "Wind_Speed", currentSpeed);
        // Add any additional data as needed
    }
    char *msg = cJSON_Print(root);
    send_mqtt("wind/topic", msg); // Replace with appropriate topic
    cJSON_Delete(root);
    return pdPASS; // or appropriate error handling
}
// Define command structur
#define CMD_WIND "wind"

static struct {
    arg_lit_t *status;
    arg_lit_t *help;
    arg_end_t *end;
} wind_args;

static void print_help() {
	arg_print_syntax(stdout, (void*) &wind_args, "\r\n");
	arg_print_glossary(stdout, (void*) &wind_args, "%-25s %s\r\n");
}

// Command function
static int cmd_wind(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **) &wind_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wind_args.end, argv[0]);
        return 1;
    }

    if (wind_args.status->count > 0) {
        // Implement logic to display wind status
    }

    if (wind_args.help->count > 0) {
    	print_help();
    	return 0;

    }

    return 0;
}

// Register command
static int register_wind_cmd() {
    wind_args.status = arg_lit0(NULL, "status", "Get wind sensor status");
    wind_args.help = arg_lit0(NULL, "help", "Show help message");
    wind_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "wind",
        .help = "Wind sensor commands",
        .hint = NULL,
        .func = &cmd_wind,
        .argtable = &wind_args
    };

    return esp_console_cmd_register(&cmd) ;
}

