#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "utils.h"
#include "storage.h"
#include "core.h"
#include "network.h"
#include "webServer.h"
#include "ftp.h"
#include "mqtt.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    initStepper();    

    if (initStorage() != ESP_OK) {
        return;
    }
    
    if (createSemaphore() != ESP_OK) {
        ESP_LOGE(TAG, "Error while creating semaphore!");
    }

    esp_err_t res = loadConfig();
    // if (res != ESP_OK) {
    //     ESP_LOGE(TAG, "Can't init config!");
    //     return;
    // }
    initServiceTask();
    initNetwork();    
    initWebServer();
    // initFTP();    
}


