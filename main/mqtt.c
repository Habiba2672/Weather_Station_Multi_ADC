/*
 * mqtt.c
 *
 * Created on: Dec 3, 2023
 * Author: alex technology
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "argtable3/argtable3.h"
#include "esp_console.h"

#include "mac_address.h"
#include "mqtt.h"
#include "sdkconfig.h"
#include "console.h"

#undef LOG_LOCAL_LEVEl
#define LOG_LEVEL ESP_LOG_DEBUG
#define TAG "mqtt"

static esp_mqtt_client_handle_t client; // modify
static bool connected = false; // modify

static struct {
	uint32_t connected;
	uint32_t disconnected;
	uint32_t subscribed;
	uint32_t unsubscribed;
	uint32_t published;
	uint32_t data;
	uint32_t errors;
} statistics;

void register_cmd();

static void log_error_if_nonzero(const char *message, int error_code) {
	if (error_code != 0) {
		ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
	}
}

BaseType_t send_mqtt(const char *topic, char *msg) {
	if (!(connected && topic && *topic && msg && *msg))
		return pdFAIL;

	return esp_mqtt_client_publish(client, topic, msg, strlen(msg), 0, 0)
			== ESP_OK ? pdTRUE : pdFALSE;
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
		int32_t event_id, void *event_data) {
	ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
	esp_mqtt_event_handle_t event = event_data;
	switch ((esp_mqtt_event_id_t) event_id) {
	case MQTT_EVENT_CONNECTED:
		++statistics.connected;
		ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
		connected = true; // TO dO
		break;
	case MQTT_EVENT_DISCONNECTED:
		++statistics.disconnected;
		ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
		connected = false; // TO DO
		break;

	case MQTT_EVENT_SUBSCRIBED:
		++statistics.subscribed;
		ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_UNSUBSCRIBED:
		++statistics.unsubscribed;
		ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_PUBLISHED:
		++statistics.published;
		ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_DATA:
		++statistics.data;
		ESP_LOGI(TAG, "MQTT_EVENT_DATA");
		printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
		printf("DATA=%.*s\r\n", event->data_len, event->data);
		break;
	case MQTT_EVENT_ERROR:
		++statistics.errors;
		ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
		if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
			log_error_if_nonzero("reported from esp-tls",
					event->error_handle->esp_tls_last_esp_err);
			log_error_if_nonzero("reported from tls stack",
					event->error_handle->esp_tls_stack_err);
			log_error_if_nonzero("captured as transport's socket errno",
					event->error_handle->esp_transport_sock_errno);
			ESP_LOGI(TAG, "Last errno string (%s)",
					strerror(event->error_handle->esp_transport_sock_errno));

		}
		break;
	default:
		ESP_LOGI(TAG, "Other event id:%d", event->event_id);
		break;
	}
}

BaseType_t mqtt_init() {
	char buffer[30];
	if (mac_get_address(buffer, sizeof(buffer)) != pdPASS)
		return pdFAIL;

	esp_mqtt_client_config_t mqtt_cfg = {
			.broker.address.uri =CONFIG_BROKER_URL,
			.credentials = { .username =CONFIG_MQTT_USER_NAME,
					.client_id = buffer,
					.authentication = {
							.password = CONFIG_MQTT_PASSWORD
				}
			}
	};
#if CONFIG_BROKER_URL_FROM_STDIN
	    char line[128];

	    if (strcmp(mqtt_cfg.broker.address.uri, "FROM_STDIN") == 0) {
	        int count = 0;
	        printf("Please enter url of mqtt broker\n");
	        while (count < 128) {
	            int c = fgetc(stdin);
	            if (c == '\n') {
	                line[count] = '\0';
	                break;
	            } else if (c > 0 && c < 127) {
	                line[count] = c;
	                ++count;
	            }
	            vTaskDelay(10 / portTICK_PERIOD_MS);
	        }
	        mqtt_cfg.broker.address.uri = line;
	        printf("Broker url: %s\n", line);
	    } else {
	        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
	        abort();
	    }
	#endif /* CONFIG_BROKER_URL_FROM_STDIN */

	esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
	/* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
	esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler,
			NULL);
	esp_mqtt_client_start(client);
	register_cmd();
	return pdPASS;
}

BaseType_t mqtt_deinit() {
	return pdPASS;
}

#define CMD_MQTT "mqtt"

static struct {
	arg_lit_t *arg_help;
	arg_lit_t *stat;
	arg_str_t *subscribe;
	arg_str_t *un_subscribe;
	arg_str_t *publish;
	arg_end_t *end;

} mqtt_arg;

static void help() {
	arg_print_syntax(stdout, (void*) &mqtt_arg, "\r\n");
	arg_print_glossary(stdout, (void*) &mqtt_arg, "%-25s %s\r\n");
}

static void print_stat() {
	printf("========== statistics ==========\r\n");
	printf("connected:%"PRIu32"\t, disconnected:%"PRIu32, statistics.connected, statistics.disconnected);
	printf("subscribed:%"PRIu32"\t, unsubscribed:%"PRIu32, statistics.subscribed, statistics.unsubscribed);
	printf("published:%"PRIu32"\t, data:%"PRIu32, statistics.published, statistics.data);
	printf("errors:%"PRIu32"\r\n", statistics.errors);
	printf("connected:%s\r\n", connected ? "true" : "false");
	printf("====================\r\n");
}

 int subscribe(const char *topic) {
	if (!(topic && *topic)) {
		printf("need a topic\r\n");
		return 1;

	}
	if (!connected) {
		printf("MQTT is not connected\r\n");
		return 2;
	}
	if (esp_mqtt_client_subscribe(client, topic, 0) == ESP_OK) {
		printf("topic %s is successfully\r\n", topic);
		return 0;
	}
	printf("Cannot subscribe to topic:%s\r\n", topic);
	return 3;
}

 int un_subscribe(const char *topic) {
	if (!(topic && *topic)) {
		printf("need a topic\r\n");
		return 1;

	}
	if (!connected) {
		printf("MQTT is not connected\r\n");
		return 2;
	}
	if (esp_mqtt_client_unsubscribe(client, topic) == ESP_OK) {
		printf("topic %s is successfully\r\n", topic);
		return 0;
	}
	printf("Cannot unsubscribe to topic:%s\r\n", topic);
	return 3;

}

 int publish(const char *topic, const char *msg) {
	if (!(topic && *topic && msg && *msg)) {
		printf("need a topic and a message\r\n");
		return 1;
	}
	if (!connected) {
		printf("MQTT is not connected\r\n");
		return 2;
	}
	int msg_id = esp_mqtt_client_publish(client, topic, msg, strlen(msg), 0, 0);
	if (msg_id == -1) {
		printf("cannot publish the message %s to %s\r\n", msg, topic);
		return 3;
	}
	printf("Message %s successfully sent to %s\r\n", msg, topic);
	return 0;
}

static int cmd_mqtt(int argc, char **argv) {
	int errors = arg_parse(argc, argv, (void*) &mqtt_arg);
	if (errors > 0) {
		arg_print_errors(stdout, (void *)&mqtt_arg.end, argv[0]);
		return 1;
	}
	if (mqtt_arg.arg_help->count > 0) {
		help();
		return 0;
	}
	if (mqtt_arg.stat->count > 0) {
		print_stat();
		return 0;
	}
	if (mqtt_arg.subscribe->count > 0) {
		return subscribe(mqtt_arg.subscribe->sval[0]);
	}
	if (mqtt_arg.un_subscribe->count > 0) {
		return un_subscribe(mqtt_arg.un_subscribe->sval[0]);
	}
	if (mqtt_arg.publish->count > 0) {
		return publish(mqtt_arg.publish->sval[0], mqtt_arg.publish->sval[1]);

	}
	return 1;
}

void register_mqtt_cmd() {
	mqtt_arg.arg_help = arg_litn("h", "help", 0, 1, "print this is help");
	mqtt_arg.stat = arg_litn(NULL, "stat", 0, 1, "print statistics");
	mqtt_arg.subscribe = arg_strn("s", "subscribe", "<s>", 1, 1,
			"subscribe to a topic");
	mqtt_arg.un_subscribe = arg_strn("s", "unsubscribe", "<s>", 1, 1,
			"unsubscribe to a topic");
	mqtt_arg.publish = arg_strn("p", "publish", "<s>", 2, 2,
			"publish a message");
	mqtt_arg.end = arg_end(0);
	esp_console_cmd_t cmd = { .command = CMD_MQTT, .help =
			"command for mqtt subsystem", .hint = "stat, subscribe, publish etc",
			.func = cmd_mqtt };

	esp_console_cmd_register(&cmd);
}
