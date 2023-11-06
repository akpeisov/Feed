//MQTT

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "core.h"
#include "cJSON.h"

static const char *TAG = "MQTT";
esp_mqtt_client_handle_t mqttclient;
bool mqtt_connected = false;

static void log_error_if_nonzero(const char * message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

void parseTopic(char* topic, char* data) {
    ESP_LOGW(TAG, "parseTopic %s %s", topic, data);
    
    SemaphoreHandle_t sem = getSemaphore();
    cJSON *jData = cJSON_Parse(data);
    if (strstr(topic, "/json")) {
        if (!cJSON_IsObject(jData)) {
            ESP_LOGE(TAG, "data is not a json");
            return;
        }
        /*
            Для JSON формата будет такой пример
            TOPIC=Test1/in/json
            DATA={
             "feed": 1
            }
        */
        ESP_LOGI(TAG, "it's json");
        if (cJSON_IsBool(cJSON_GetObjectItem(jData, "feed")) && cJSON_IsTrue(cJSON_GetObjectItem(jData, "feed"))) {
            if (xSemaphoreTake(sem, portMAX_DELAY) == pdTRUE) {
                setFeedFlag(data);
                xSemaphoreGive(sem);
            }
        }
        cJSON_Delete(jData);
    } else if (strstr(topic, "/feed") && strstr(data, "ON")) {
        setFeedFlag(NULL);
    }                         
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id = 0;
    // your_context_t *context = event->context;    
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED. Hostname %s", getNetworkConfigValueString("hostname"));            
            //msg_id = esp_mqtt_client_subscribe(client, "/main/#", 0);
            char stopic[100];
            //strcpy(stopic, "/");
            strcpy(stopic, getNetworkConfigValueString("hostname"));
            strcat(stopic, "/in/#\0");
            msg_id = esp_mqtt_client_subscribe(client, stopic, 0);
            mqtt_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            mqtt_connected = false;
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            //msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
            //ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            // printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            // printf("DATA=%.*s\r\n", event->data_len, event->data);            
            // parse topic
            // break;
            // char topic[100];
            // strncpy(topic, event->topic, event->topic_len);
            // topic[event->topic_len] = 0;
            // char data[200];
            // strncpy(data, event->data, event->data_len);
            // data[event->data_len] = 0;
            char *topic = (char*)malloc(event->topic_len+1);
            strncpy(topic, event->topic, event->topic_len);
            topic[event->topic_len] = 0;
            char *data = (char*)malloc(event->data_len+1);
            strncpy(data, event->data, event->data_len);
            data[event->data_len] = 0;            
            parseTopic(topic, data);
            free(topic);
            free(data);
            // char *topic = malloc(event->topic_len+1);
            // strcpy(topic, event->topic);
            // topic[event->topic_len] = 0;
            // ESP_LOGI(TAG, "topic %s, data %s", topic, data);
            // parseTopic(topic, data);            
            // free(topic);
            // free(data);
            //parseTopic(event->topic, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
                log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
                log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
                ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

            }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

void mqttPublish(char* topic, char* data) {
    if (mqtt_connected)
        esp_mqtt_client_publish(mqttclient, topic, data, 0, 0, 0);    
}

void mqtt_app_start(void)
{
    if (!getNetworkConfigValueBool2("mqtt", "enabled")) {
        ESP_LOGI(TAG, "No need to init MQTT");
        return;
    }    
    esp_mqtt_client_config_t mqtt_cfg = {    
        .uri = "mqtt://"
    };
    mqtt_cfg.uri = getNetworkConfigValueString2("mqtt", "url");
    if (mqtt_cfg.uri == NULL) {
        ESP_LOGE(TAG, "No MQTT uri defined");
        return;
    }
    
    //esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    mqttclient = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqttclient, ESP_EVENT_ANY_ID, mqtt_event_handler, mqttclient);
    esp_mqtt_client_start(mqttclient);
}

void initMQTT() {
    mqtt_app_start();
}
