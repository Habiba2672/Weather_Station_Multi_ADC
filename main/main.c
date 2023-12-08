
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_vfs_dev.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"


#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "ad.h"
#include "console.h"
#include "wind.h" // Include the header for wind sensor
#include "mqtt.h"

#define TAG "main"

void app_main(void)
{
	 ESP_LOGI(TAG, "[APP] Startup..");
	    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
	    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

	    esp_log_level_set("*", ESP_LOG_INFO);
	    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
	    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
	    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
	    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
	    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
	    esp_log_level_set("outbox", ESP_LOG_VERBOSE);
	    register_cmd();

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(example_connect());

    configASSERT(con_init()); // You might mean con_init() here to initialize the console

    configASSERT(mqtt_init());

    //configASSERT(ad_init()); // Initialize AD module
    BaseType_t result = ad_init();

    if(result != pdPASS) {
        ESP_LOGE(TAG, "Error initializing A/D!");
        return;
    }
    configASSERT(wind_init()); // Initialize Wind module

}
