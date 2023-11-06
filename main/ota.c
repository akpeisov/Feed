// ota.c

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "core.h"
#include "storage.h"

static const char *TAG = "OTA";
// extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
// extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

#define OTA_URL_SIZE 256

bool taskState = false;

char* getCurrentVersion() {
    esp_app_desc_t running_app_info;
    char* ver = malloc(sizeof(running_app_info.version));
    bzero(ver, sizeof(running_app_info.version));
    const esp_partition_t *running = esp_ota_get_running_partition();    
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {        
        memcpy(ver, running_app_info.version, sizeof(running_app_info.version));
    }
    return ver;
}

static esp_err_t validate_image_header(esp_app_desc_t *new_app_info)
{
    if (new_app_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_info;
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
    }

    if (memcmp(new_app_info->version, running_app_info.version, sizeof(new_app_info->version)) == 0) {
        ESP_LOGW(TAG, "Current running version is the same as a new. We will not continue the update.");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void otaTask(void *pvParameter)
{
    // wait 20 seconds for network ready
    vTaskDelay(5000 / portTICK_RATE_MS);
    ESP_LOGI(TAG, "Starting OTA update");
    char* url = getNetworkConfigValueString("otaurl");
    if (url == NULL) {
        ESP_LOGE(TAG, "No URL for OTA defined!");
        taskState = false;
        vTaskDelete(NULL);
        goto ota_end2;
    }
    esp_err_t ota_finish_err = ESP_OK;
    char * buffer;
    if (loadTextFile("/config/ca_cert.pem", &buffer) != ESP_OK) {
        ESP_LOGE(TAG, "Can't load certificate!");
        taskState = false;
        vTaskDelete(NULL);
        goto ota_end2;
    }
    esp_http_client_config_t config = {
        .url = url,
        // .cert_pem = (char *)server_cert_pem_start,
        .cert_pem = buffer,
        .timeout_ms = 5000,
    };

    config.skip_cert_common_name_check = true;

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed");
        taskState = false;
        vTaskDelete(NULL);
    }

    esp_app_desc_t app_desc;
    err = esp_https_ota_get_img_desc(https_ota_handle, &app_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_read_img_desc failed");
        goto ota_end;
    }    
    err = validate_image_header(&app_desc);
    if (err != ESP_OK) {
        //ESP_LOGE(TAG, "image header verification failed");
        // TODO : reset or restore normal mode???
        goto ota_end;
    }

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        // esp_https_ota_perform returns after every read operation which gives user the ability to
        // monitor the status of OTA upgrade by calling esp_https_ota_get_image_len_read, which gives length of image
        // data read so far.
        ESP_LOGD(TAG, "Image bytes read: %d", esp_https_ota_get_image_len_read(https_ota_handle));
    }

    if (esp_https_ota_is_complete_data_received(https_ota_handle) != true) {
        // the OTA image was not completely received and user can customise the response to this situation.
        ESP_LOGE(TAG, "Complete data was not received.");
    }

ota_end:
    taskState = false;
    ota_finish_err = esp_https_ota_finish(https_ota_handle);
    if ((err == ESP_OK) && (ota_finish_err == ESP_OK)) {
        ESP_LOGI(TAG, "ESP_HTTPS_OTA upgrade successful. Rebooting ...");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        }
        ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed %d", ota_finish_err);
        vTaskDelete(NULL);
    }
ota_end2:    
    vTaskDelete(NULL);
}

void startOTA() {
    if (taskState) {
        ESP_LOGE(TAG, "OTA update already running");
        return;
    }
    taskState = true;    
    xTaskCreate(&otaTask, "otaTask", 1024 * 8, NULL, 5, NULL);
}

