#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "mqtt.h"
#include "mac_address.h"

#include "esp_mac.h"
#include "time.h"

#include "ad.h"

#define DEBUG // TODO dont't forget to set undef!!!

#define MAX_CHANNELS 2

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#define TAG "ad"

#define TOPIC_TEMPERATURES "sensor/temperature"

#define CHAN0          ADC_CHANNEL_1
#define CHAN2         ADC_CHANNEL_1

#define ADC_ATTEN           ADC_ATTEN_DB_11

#define MOVING_AVG_QUEUE_SIZE 5

#define MOVING_HIST_DELTA 8

#define AD_CONVERSION_MS 100

#define AD_RAW_MIN 0
#define AD_RAW_MAX 4096

#define TEMP_MIN -30 //Celcius
#define TEMP_MAX 40

#define NVS_KEY_D0 "d0"
#define NVS_KEY_D1 "d1"
#define NVS_KEY_T0 "t0"
#define NVS_KEY_T1 "t1"
#define NVS_KEY_TG_ALPHA "ta"
#define NVS_NAME_SPACE "ad_storage"

static adc_oneshot_unit_handle_t adc_handle;

static TaskHandle_t tsk_ad_handle=NULL;

typedef struct {
	uint16_t avg[MOVING_AVG_QUEUE_SIZE];
	uint8_t index;

} moving_avg_t;

typedef struct {
	uint16_t min;
	uint16_t max;
	uint16_t delta;
} moving_hist_t;

typedef struct {
	uint16_t d0;
	uint16_t d1;
	float temp_min;
	float temp_max;
	float tg_alpha;
	bool valid;
} calibration_t;

static struct {
	int raw; // read directly from sd
	uint16_t normalized; // after moving avg and hist and calibration
	float thermal_value;
	moving_avg_t moving_avg;
	moving_hist_t moving_hist;
	uint32_t ad_errors;
	calibration_t calibration;
	SemaphoreHandle_t semaphore;
	uint32_t semaphore_error;

}ad[MAX_CHANNELS];

void register_cmd();

static inline int chk_ch(int ch) {
	if (ch < 0 || ch >= MAX_CHANNELS) {
		printf("channel must be between o up to %d\r\n", MAX_CHANNELS);
		return 0;
	}
	return 1;
}

static uint16_t moving_hist(uint16_t in, int channel) {
	if (in >= ad[channel].moving_hist.min && in <= ad[channel].moving_hist.max)
		return ad[channel].moving_hist.min;

	ad[channel].moving_hist.max =
			(in + ad[channel].moving_hist.delta) >= AD_RAW_MAX ?
					AD_RAW_MAX : in + ad[channel].moving_hist.delta;
	ad[channel].moving_hist.min =
			((int) in) - ((int) ad[channel].moving_hist.delta) <= 0 ?
					AD_RAW_MIN : in - MOVING_HIST_DELTA;

	return ad[channel].moving_hist.min;
}

static uint16_t moving_avg(uint16_t in, int channel) {
	ad[channel].moving_avg.avg[ad[channel].moving_avg.index] = in;
	ad[channel].moving_avg.index = (ad[channel].moving_avg.index + 1)
			% MOVING_AVG_QUEUE_SIZE;

	uint32_t sum = 0;
	for (uint8_t i = 0; i < MOVING_AVG_QUEUE_SIZE; i++)
		sum += ad[channel].moving_avg.avg[i];

	return sum / MOVING_AVG_QUEUE_SIZE;
}

static void calc_temp(uint16_t in, int channel) {
	if (!ad[channel].calibration.valid)
		return;

	float tmp = in * ad[channel].calibration.tg_alpha;
	if (xSemaphoreTake(ad[channel].semaphore, pdMS_TO_TICKS(10)) == pdPASS) {
		ad[channel].thermal_value = tmp;
		xSemaphoreGive(ad[channel].semaphore);
	} else {
		++ad[channel].semaphore_error;
	}
}

// read value from ADC, process and calculate the corresponding temp
static void tsk_ad(void *p) {
    ESP_LOGD(TAG, "Enter tsk_ad");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(AD_CONVERSION_MS));
        for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
            adc_channel_t adc_ch = (ch == 0) ? CHAN0 : CHAN2;
            // Check if the channel is calibrated in debug mode
            #ifndef DEBUG
            if (!ad[ch].calibration.valid) {
                ESP_LOGE(TAG, "A/D channel %d is not calibrated, please calibrate it!", ch);
                continue;
            }
            #endif

            if (adc_oneshot_read(adc_handle, adc_ch, &ad[ch].raw) != ESP_OK) {
                ++ad[ch].ad_errors;
                continue;
            }

            // Process the raw ADC value
            ad[ch].normalized = moving_hist(moving_avg(ad[ch].raw, ch), ch);
            calc_temp(ad[ch].normalized, ch);
            ESP_LOGI(TAG, "Chan%d - raw:%u, normalized:%u, temp:%f", ch, ad[ch].raw, ad[ch].normalized, ad[ch].thermal_value);
        }
    }
}


static int validate_calibrate(int channel) {
    if (!chk_ch(channel))

        return 0;

    calibration_t *calib = &ad[channel].calibration;

    return (calib->d0 <= AD_RAW_MAX)
            && (calib->d1 <= AD_RAW_MAX)
            && calib->d1 > calib->d0
            && calib->temp_min >= TEMP_MIN
            && calib->temp_max <= TEMP_MAX
            && calib->temp_max > calib->temp_min;
}


static void restore_flash(int channel) {
	if (!chk_ch(channel))
		return;

	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAME_SPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) {
		printf("Nobody calibrated this channel (%d) yet\r\n", channel);
		return;
	}

	if (nvs_get_u16(handle, NVS_KEY_D0, &ad[channel].calibration.d0) != ESP_OK) {
		goto exit;
	}

	if (nvs_get_u16(handle, NVS_KEY_D1, &ad[channel].calibration.d1) != ESP_OK) {
		goto exit;
	}

	uint16_t v;
	if (nvs_get_u16(handle, NVS_KEY_T0, &v) != ESP_OK) {
		goto exit;
	}
	ad[channel].calibration.temp_min = v / -30.0;

	if (nvs_get_u16(handle, NVS_KEY_T1, &v) != ESP_OK) {
		goto exit;
	}
	ad[channel].calibration.temp_max = v / 40.0;

	ad[channel].calibration.valid = false;

	uint16_t divider = ad[channel].calibration.d1 - ad[channel].calibration.d0;
	if (divider == 0)
		goto exit;

	ad[channel].calibration.tg_alpha = (ad[channel].calibration.temp_max
			- ad[channel].calibration.temp_min) / divider;
	validate_calibrate(channel);

	exit: nvs_close(handle);
}

BaseType_t ad_init() {
    esp_log_level_set(TAG, LOG_LOCAL_LEVEL);
    //bzero(&ad, sizeof(ad_t) * MAX_CHANNELS);
    // ADC initialization

    BaseType_t taskCreated = xTaskCreate(tsk_ad, "A/D", 8192, NULL, uxTaskPriorityGet(NULL), &tsk_ad_handle);

    if(taskCreated != pdPASS) {
        ESP_LOGE(TAG, "Failed to create A/D task. Error code: %d", taskCreated);
        return taskCreated;
    }

    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = { .bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_11 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CHAN0, &config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CHAN2, &config)); // Assuming CHAN2 is the second channel

    for (int i = 0; i < MAX_CHANNELS; i++) {
        ad[i].moving_avg.index = 0;
        memset(ad[i].moving_avg.avg, 0, sizeof(ad[i].moving_avg.avg));

        ad[i].moving_hist.min = AD_RAW_MIN;
        ad[i].moving_hist.max = AD_RAW_MAX;
        ad[i].moving_hist.delta = MOVING_HIST_DELTA;
        ad[i].semaphore = xSemaphoreCreateMutex();
        if (ad[i].semaphore == NULL) {
            ESP_LOGE(TAG, "Failed to create semaphore for channel %d", i);
        }
    }


    register_cmd();

    for (int i = 0; i < MAX_CHANNELS; i++) {
        restore_flash(i);
    }

    return pdPASS;
}


BaseType_t ad_deinit() {

	if(tsk_ad_handle !=NULL){
		vTaskDelete(tsk_ad_handle);
	}
	return pdPASS;
}

// retrieve the lates calculated temperature value form the ADC module.

BaseType_t ad_get(float *value, TickType_t wait, int channel) {
	if (!value || !chk_ch(channel))
		return pdFAIL;
	if (xSemaphoreTake(ad[channel].semaphore, wait) == pdPASS) {
		*value = ad[channel].thermal_value;
		xSemaphoreGive(ad[channel].semaphore);
		return pdPASS;
	}
	return pdFAIL;
}

extern BaseType_t send_mqtt(const char *topic, char *msg);

BaseType_t ad_send() {
	BaseType_t result = pdFAIL;
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		ESP_LOGW(TAG, "Failed to create JSON root object");
		return pdFAIL;
	}
	typedef union {
		uint8_t as_bytes[6];
		uint64_t as_long;
	} mac_t;

	mac_t mac;

	esp_efuse_mac_get_default(mac.as_bytes);
	char macAddressStr[30];
	snprintf(macAddressStr, sizeof(macAddressStr), "%lld", mac.as_long);

	time_t now;
	time(&now);
	struct tm *timeinfo = gmtime(&now);
	char timestampStr[30];
	strftime(timestampStr, sizeof(timestampStr), "%Y-%m-%dT%H:%M:%SZ",
			timeinfo);

	if (!cJSON_AddStringToObject(root, "Timestamp_ISO8601", timestampStr)) {
		ESP_LOGE(TAG, "Error adding timestamp to JSON");
		goto exit;
	}

	float valueInner, valueOuter;
	if (ad_get(&valueInner, pdMS_TO_TICKS(100), 0) == pdPASS) { // For inner channel (channel 0)
		cJSON_AddNumberToObject(root, "Temperature_Inner", valueInner);
	} else {
		ESP_LOGW(TAG, "Failed to get data for inner channel");
	}

	if (ad_get(&valueOuter, pdMS_TO_TICKS(100), 1) == pdPASS) { // For outer channel (channel 1)
		cJSON_AddNumberToObject(root, "Temperature_Outer", valueOuter);
	} else {
		ESP_LOGW(TAG, "Failed to get data for outer channel");
	}

	char *str = cJSON_Print(root);
	if (!str) {
		ESP_LOGE(TAG, "Failed to print JSON string");
		goto exit;
	}

	// Send to MQTT
	ESP_LOGD(TAG, "Sending JSON string: %s", str);
	result = send_mqtt(TOPIC_TEMPERATURES, str);

	exit: cJSON_Delete(root);
	return result;
}

#define CMD_AD "ad"

//ad [--channel <number>] [--stat <--channel>] [--calibrate <--channel> --point <do|d1> --temp <double>] [--help][--flash <-channel>]

static struct {
	arg_lit_t *help;
	arg_int_t *channel;
	arg_lit_t *stat;
	arg_str_t *point;
	arg_dbl_t *temp;
	arg_lit_t *flash;
	arg_lit_t *calibrate;
	arg_end_t *end;
} arg_ad;

static void print_help() {
	arg_print_syntax(stdout, (void*) &arg_ad, "\r\n");
	arg_print_glossary(stdout, (void*) &arg_ad, "%-25s %s\r\n");
}

static void print_avg(int channel) {
	if (!chk_ch(channel))
		return;

	printf("Moving avg, index:%d", ad[channel].moving_avg.index);
	for (int i = 0; i < MOVING_AVG_QUEUE_SIZE; i++)
		printf("%u%s", ad[channel].moving_avg.avg[i],
				i < MOVING_AVG_QUEUE_SIZE - 1 ? ", " : "\r\n");
}

static void print_stat(int channel) {
	if (!chk_ch(channel))
		return;

	printf("raw: %4d\tnormalized:%4d\tthermal:%5.2f\r\n", ad[channel].raw,
			ad[channel].normalized, ad[channel].thermal_value);
	printf("errors: %lu\r\n", ad[channel].ad_errors);
	print_avg(channel);
	printf("Moving hist min: %d, max:%d, delta:%d\r\n",
			ad[channel].moving_hist.min, ad[channel].moving_hist.max,
			ad[channel].moving_hist.delta);
	printf("Calibration do:%d, d1:%d, t0:%f, t1:%f, tg alpha:%f, valid:%s\r\n",
			ad[channel].calibration.d0, ad[channel].calibration.d1,
			ad[channel].calibration.temp_min, ad[channel].calibration.temp_max,
			ad[channel].calibration.tg_alpha,
			ad[channel].calibration.valid ? "true" : "false");
}
static void print_flash(int channel) {
	if (!chk_ch(channel))
		return;

	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAME_SPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) {
		printf("Nobody calibrated this channel (%d) yet\r\n", channel);
		return;
	}

	uint16_t d0;
	if (nvs_get_u16(handle, NVS_KEY_D0, &d0) != ESP_OK) {
		goto exit;

	}
	uint16_t d1;
	if (nvs_get_u16(handle, NVS_KEY_D1, &d1) != ESP_OK) {
		goto exit;
	}

	uint16_t t0;
	if (nvs_get_u16(handle, NVS_KEY_T0, &t0) != ESP_OK) {
		goto exit;

	}

	uint16_t t1;
	if (nvs_get_u16(handle, NVS_KEY_T1, &t1) != ESP_OK) {
		goto exit;

	}

	uint16_t ta;
	if (nvs_get_u16(handle, NVS_KEY_TG_ALPHA, &ta) != ESP_OK) {
		goto exit;

	}

	printf("d0:%d. d1:%d, t0:%f t1:%f , tg alpha:%f\r\n", d0, d1, t0 / -30.0,
			t1 / 40.0, ta / 100.0);

	exit: nvs_close(handle);
}

static void store_calibrate(int channel, float temp, int point) {
    if (!(chk_ch(channel)) || point < 0 || point > 1 || temp < TEMP_MIN || temp > TEMP_MAX)
        return;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAME_SPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        printf("Cannot open %s to read/write\r\n", NVS_NAME_SPACE);
        return;
    }

    int v;
    // Choose the correct ADC channel based on input channel
    adc_channel_t adc_channel = (channel == 0) ? CHAN0 : CHAN2;
    adc_oneshot_read(adc_handle, adc_channel, &v);

    // Use different NVS keys for different channels
    const char *key_d = (point == 0) ? NVS_KEY_D0 : NVS_KEY_D1;
    const char *key_t = (point == 0) ? NVS_KEY_T0 : NVS_KEY_T1;

    if (nvs_set_u16(handle, key_d, v) != ESP_OK) {
        printf("Cannot write %s value to flash\r\n", key_d);
        goto exit;
    }

    if (nvs_set_u16(handle, key_t, (uint16_t) temp * 100) != ESP_OK) {
        printf("Cannot write %s value to flash\r\n", key_t);
        goto exit;
    }

exit:
    nvs_close(handle);
}

static int calibrate(int channel, const char *point, float temp) {
    if (!chk_ch(channel))
        return 1;

    if (temp < TEMP_MIN || temp > TEMP_MAX) {
        printf("Temperature must be between %f and %f\r\n", TEMP_MIN / -30.0, TEMP_MAX / 40.0);
        return 1;
    }

    int pointIndex = -1;
    if (strcasecmp("d0", point) == 0) {
        pointIndex = 0;
    } else if (strcasecmp("d1", point) == 0) {
        pointIndex = 1;
    }

    if (pointIndex != -1) {
        store_calibrate(channel, temp, pointIndex);
        return validate_calibrate(channel);
    }

    printf("Which point do you want to calibrate? (d0 or d1)\r\n");
    return 1;
}

// cmd_ad command handler
static int cmd_ad(int argc, char **argv) {
    int errs = arg_parse(argc, argv, (void*)&arg_ad);
    if (errs) {
        arg_print_errors(stderr, arg_ad.end, argv[0]);
        return 1; // Error in parsing arguments
    }

    // Get channel
    int channel = *arg_ad.channel->ival;

    // Validate channel
    if (channel != 0 && channel != 2) {
        printf("Channel must be 0 or 2\n");
        print_help();
        return 1;
    }

    // Print status
    if (arg_ad.stat->count > 0) {
        print_stat(channel);
        return 0;
    }

    // Print flash
    if (arg_ad.flash->count > 0) {
        print_flash(channel);
        return 0;
    }
    // Calibrate
    if (arg_ad.calibrate->count > 0) {
        if (arg_ad.point->count == 0 || arg_ad.temp->count == 0) {
            printf("Point and temperature are required for calibration\n");
            return 1;
        }
        // Debugging: Print received arguments
        printf("Received channel: %d\n", *arg_ad.channel->ival);
        if(arg_ad.point->count > 0) {
            printf("Received point: %s\n", arg_ad.point->sval[0]);
        }
        if(arg_ad.temp->count > 0) {
            printf("Received temperature: %f\n", *arg_ad.temp->dval);
        }
        // Extract point value
        const char *point = arg_ad.point->sval[0];

        // Validate temp
        float temp = *arg_ad.temp->dval;
        //if (!temp)
        	if (arg_ad.temp->count == 0)
        {
            printf("Missing temp value\n");
            return 1;
        }

        // Calibrate channel
        int result = calibrate(channel, point, temp);
        if (result != 0) {
            return 1;
        }

        return 0;
    }


    print_help();

    return 0;
}

void register_cmd() {

    arg_ad.help = arg_lit0("h", "help", "print this help for A/D");
    arg_ad.channel = arg_int1("c", "channel", "<n>", "channel index");
    arg_ad.stat = arg_lit0("s", "status", "print A/D channel status");
    arg_ad.point = arg_str0("p", "point", "d0|d1",
        "min or max point of digital value");
    arg_ad.temp = arg_dbl0("t", "temp", "<f>",
        "temperature belong to digital input (d0|d1)");
    arg_ad.flash = arg_lit0("f", "flash",
        "show stored params in flash for a channel");
    arg_ad.calibrate = arg_lit0(NULL, "calibrate", "calibrate a channel");

    arg_ad.end = arg_end(20);

    esp_console_cmd_t cmd = {
            .command = CMD_AD, // Ensure this is defined correctly
            .help = "A/D commands",
            .hint = NULL,
            .func = &cmd_ad,
            .argtable = &arg_ad
        };

        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }
