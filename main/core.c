//core.c
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "storage.h"
#include "webServer.h"
#include "utils.h"
#include "freertos/semphr.h"
#include "ota.h"
#include "core.h"
#include "driver/gpio.h"
#include "mqtt.h"
#include "driver/rmt.h"

static const char *TAG = "CORE";
static cJSON *networkConfig;
static cJSON *feedConfig;

//network config defaults
#define DEF_IP          "192.168.99.9"
#define DEF_IPW         "192.168.99.10"
#define DEF_MASK        "255.255.255.0"
#define DEF_GW          "192.168.99.98"
#define DEF_DNS         "192.168.99.98"
#define DEF_WIFI_SSID   "Alana"
#define DEF_WIFI_PASS   "zxcv1234"
#define DEF_DHCP_EN     1
//service config defaults
#define DEF_NAME        "Device-0"
#define DEF_USERNAME    "admin"
#define DEF_PASSWORD    "admin"

// stepper
#define RMT_TX_CHANNEL 0
#define RMT_TX_GPIO_NUM 15
#define PIN_EN 14
#define PIN_RESET 2
#define PIN_DIR 13

#define  setbit(var, bit)    ((var) |= (1 << (bit)))
#define  clrbit(var, bit)    ((var) &= ~(1 << (bit)))

bool reboot = false;
SemaphoreHandle_t sem_busy = NULL;
bool feedFlag = false;

esp_err_t feed();

//headers
void publish(uint8_t slaveId, uint8_t outputId, uint8_t action);

esp_err_t saveFeedConfig() {
    char *feed = cJSON_Print(feedConfig);
    esp_err_t err = saveTextFile("/config/feedconfig.json", feed);
    free(feed);
    return err;
}

esp_err_t createFeedConfig() {
    feedConfig = cJSON_CreateObject();        
    cJSON_AddItemToObject(feedConfig, "speed", cJSON_CreateNumber(1000));
    cJSON_AddItemToObject(feedConfig, "stepFwd", cJSON_CreateNumber(19));
    cJSON_AddItemToObject(feedConfig, "stepRev", cJSON_CreateNumber(12));
    cJSON_AddItemToObject(feedConfig, "stepCnt", cJSON_CreateNumber(100));
    return ESP_OK;
}

esp_err_t loadFeedConfig() {
    char * buffer = NULL;
    if (loadTextFile("/config/feedconfig.json", &buffer) == ESP_OK) {
        cJSON *parent = cJSON_Parse(buffer);
        if (!cJSON_IsObject(parent)) {            
            free(buffer);
            return ESP_FAIL;
        }            
        cJSON_Delete(feedConfig);
        feedConfig = parent;                            
    } else {
        ESP_LOGI(TAG, "creating default feed config");
        cJSON_Delete(feedConfig);
        if (createFeedConfig() == ESP_OK)
            saveFeedConfig();      
    }
    free(buffer);
    return ESP_OK;    
}

esp_err_t createNetworkConfig() {
    networkConfig = cJSON_CreateObject();    
    
    cJSON *eth = cJSON_CreateObject();
    cJSON_AddItemToObject(eth, "enabled", cJSON_CreateBool(false));
    cJSON_AddItemToObject(eth, "dhcp", cJSON_CreateBool(DEF_DHCP_EN));
    cJSON_AddItemToObject(eth, "ip", cJSON_CreateString(DEF_IP));
    cJSON_AddItemToObject(eth, "netmask", cJSON_CreateString(DEF_MASK));
    cJSON_AddItemToObject(eth, "gateway", cJSON_CreateString(DEF_GW));
    cJSON_AddItemToObject(networkConfig, "eth", eth);
    // cJSON_Delete(eth);

    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddItemToObject(wifi, "enabled", cJSON_CreateBool(true));
    cJSON_AddItemToObject(wifi, "ssid", cJSON_CreateString(DEF_WIFI_SSID));
    cJSON_AddItemToObject(wifi, "pass", cJSON_CreateString(DEF_WIFI_PASS));
    cJSON_AddItemToObject(wifi, "dhcp", cJSON_CreateBool(DEF_DHCP_EN));
    cJSON_AddItemToObject(wifi, "ip", cJSON_CreateString(DEF_IPW));
    cJSON_AddItemToObject(wifi, "netmask", cJSON_CreateString(DEF_MASK));
    cJSON_AddItemToObject(wifi, "gateway", cJSON_CreateString(DEF_GW));
    cJSON_AddItemToObject(networkConfig, "wifi", wifi);
    // cJSON_Delete(wifi);

    cJSON_AddItemToObject(networkConfig, "dns", cJSON_CreateString(DEF_DNS));
    cJSON_AddItemToObject(networkConfig, "hostname", cJSON_CreateString(DEF_NAME));
    cJSON_AddItemToObject(networkConfig, "ntpserver", cJSON_CreateString("pool.ntp.org"));
    cJSON_AddItemToObject(networkConfig, "ntpTZ", cJSON_CreateString("UTC-6:00"));
    cJSON_AddItemToObject(networkConfig, "otaurl", cJSON_CreateString("https://192.168.99.6:8443/Stairs.bin"));
    
    cJSON *mqtt = cJSON_CreateObject();
    cJSON_AddItemToObject(mqtt, "enabled", cJSON_CreateBool(false));
    cJSON_AddItemToObject(mqtt, "url", cJSON_CreateString(""));    
    cJSON_AddItemToObject(networkConfig, "mqtt", mqtt);
    // cJSON_Delete(mqtt);
       
    cJSON *ftp = cJSON_CreateObject();
    cJSON_AddItemToObject(ftp, "enabled", cJSON_CreateBool(false));
    cJSON_AddItemToObject(ftp, "user", cJSON_CreateString("admin"));    
    cJSON_AddItemToObject(ftp, "pass", cJSON_CreateString("admin1"));    
    cJSON_AddItemToObject(networkConfig, "ftp", ftp);
    // cJSON_Delete(ftp);
    
    cJSON *rlog = cJSON_CreateObject();
    cJSON_AddItemToObject(rlog, "enabled", cJSON_CreateBool(false));
    cJSON_AddItemToObject(rlog, "server", cJSON_CreateString("192.168.4.2"));    
    cJSON_AddItemToObject(rlog, "port", cJSON_CreateNumber(514));    
    cJSON_AddItemToObject(networkConfig, "rlog", rlog);
    // cJSON_Delete(rlog);

    return ESP_OK;
}

esp_err_t saveNetworkConfig() {
    char *net = cJSON_Print(networkConfig);
    esp_err_t err = saveTextFile("/config/networkconfig.json", net);
    free(net);
    return err;
}

esp_err_t loadNetworkConfig() {
    char * buffer;
    if (loadTextFile("/config/networkconfig.json", &buffer) == ESP_OK) {
        cJSON *parent = cJSON_Parse(buffer);
        if(!cJSON_IsObject(parent) && !cJSON_IsArray(parent))
        {
            free(buffer);
            return ESP_FAIL;
        }
        cJSON_Delete(networkConfig);
        networkConfig = parent;        
    } else {
        ESP_LOGI(TAG, "can't read networkConfig. creating default config");
        cJSON_Delete(networkConfig);
        if (createNetworkConfig() == ESP_OK)
            saveNetworkConfig();        
    }
    free(buffer);
    return ESP_OK;
}

esp_err_t loadConfig() {
    if ((loadNetworkConfig() == ESP_OK) && 
        (loadFeedConfig() == ESP_OK)) {
        return ESP_OK;
    }    
    return ESP_FAIL;
}

uint16_t getNetworkConfigValueInt(const char* name) {
    if (!cJSON_IsNumber(cJSON_GetObjectItem(networkConfig, name)))    
        return 0;
    return cJSON_GetObjectItem(networkConfig, name)->valueint;
}

bool getNetworkConfigValueBool(const char* name) {
    return cJSON_IsTrue(cJSON_GetObjectItem(networkConfig, name));
}

char *getNetworkConfigValueString(const char* name) {
    if (!cJSON_IsString(cJSON_GetObjectItem(networkConfig, name)))    
        return NULL;
    return cJSON_GetObjectItem(networkConfig, name)->valuestring;
}

uint16_t getNetworkConfigValueInt2(const char* parentName, const char* name) {
    if (!cJSON_IsNumber(cJSON_GetObjectItem(cJSON_GetObjectItem(networkConfig, parentName), name)))
        return 0;
    return cJSON_GetObjectItem(cJSON_GetObjectItem(networkConfig, parentName), name)->valueint;
}

bool getNetworkConfigValueBool2(const char* parentName, const char* name) {
    return cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(networkConfig, parentName), name));
}

char *getNetworkConfigValueString2(const char* parentName, const char* name) {
    if (!cJSON_IsString(cJSON_GetObjectItem(cJSON_GetObjectItem(networkConfig, parentName), name)))
        return NULL;
    return cJSON_GetObjectItem(cJSON_GetObjectItem(networkConfig, parentName), name)->valuestring;
}

void setErrorTextJson(char **response, const char *text, ...) {
    char dest[1024]; // maximum lenght
    va_list argptr;
    va_start(argptr, text);
    vsprintf(dest, text, argptr);
    va_end(argptr);
    *response = (char*)malloc(strlen(dest)+1+13);
    strcpy(*response, "{\"error\": \"");
    strcat(*response, dest);
    strcat(*response, "\"}");
    ESP_LOGE(TAG, "%s", dest);
}

void setTextJson(char **response, const char *text, ...) {
    char dest[1024]; 
    va_list argptr;
    va_start(argptr, text);
    vsprintf(dest, text, argptr);
    va_end(argptr);
    *response = (char*)malloc(strlen(dest)+1+16);
    strcpy(*response, "{\"response\": \"");
    strcat(*response, dest);
    strcat(*response, "\"}");
    ESP_LOGI(TAG, "%s", dest);
}

void setErrorText(char **response, const char *text, ...) {
    char dest[1024]; // maximum lenght
    va_list argptr;
    va_start(argptr, text);
    vsprintf(dest, text, argptr);
    va_end(argptr);
    *response = (char*)malloc(strlen(dest)+1);
    strcpy(*response, dest);
    ESP_LOGE(TAG, "%s", dest);
}

void setText(char **response, const char *text, ...) {
    char dest[1024]; 
    va_list argptr;
    va_start(argptr, text);
    vsprintf(dest, text, argptr);
    va_end(argptr);
    *response = (char*)malloc(strlen(dest)+1);
    strcpy(*response, dest);
    ESP_LOGI(TAG, "%s", dest);
}

esp_err_t getNetworkConfig(char **response) {
    ESP_LOGI(TAG, "getNetworkConfig");
    if (!cJSON_IsObject(networkConfig) && !cJSON_IsArray(networkConfig)) {      
        setErrorText(response, "networkConfig is not a json");
        return ESP_FAIL;
    }    
    *response = cJSON_Print(networkConfig);
    return ESP_OK;    
}

esp_err_t setNetworkConfig(char **response, char *content) {
    cJSON *parent = cJSON_Parse(content);
    if(!cJSON_IsObject(parent))
    {
        setErrorText(response, "Is not a JSON object");
        cJSON_Delete(parent);
        return ESP_FAIL;
    }
        
    cJSON_Delete(networkConfig);
    networkConfig = parent;    
    saveNetworkConfig();
    setTextJson(response, "OK");
    return ESP_OK;
}

esp_err_t getFeedConfig(char **response) {
    ESP_LOGI(TAG, "getFeedConfig");
    if (!cJSON_IsObject(feedConfig) && !cJSON_IsArray(feedConfig)) {      
        setErrorText(response, "feedConfig is not a json");
        return ESP_FAIL;
    }    
    *response = cJSON_Print(feedConfig);
    return ESP_OK;    
}

esp_err_t setFeedConfig(char **response, char *content) {
    cJSON *parent = cJSON_Parse(content);
    if(!cJSON_IsObject(parent))
    {
        setErrorText(response, "Is not a JSON object");
        cJSON_Delete(parent);
        return ESP_FAIL;
    }
        
    cJSON_Delete(feedConfig);
    feedConfig = parent;    
    saveFeedConfig();
    setTextJson(response, "OK");
    return ESP_OK;
}
esp_err_t factoryReset() {
    cJSON_Delete(networkConfig);
    createNetworkConfig();    
    saveNetworkConfig();      
    return ESP_OK;
}

esp_err_t setFactoryReset(char **response) {     
    ESP_LOGI(TAG, "setFactoryReset"); 
    factoryReset();
    setTextJson(response, "OK");
    return ESP_OK;  
}

esp_err_t getDeviceInfo(char **response) {
    //ESP_LOGI(TAG, "getStatus");
    cJSON *status = cJSON_CreateObject();    
    char *uptime = getUpTime();
    char *curdate = getCurrentDateTime("%d.%m.%Y %H:%M:%S");
    char *hostname = getNetworkConfigValueString("hostname");
    char *version = getCurrentVersion();
    cJSON_AddItemToObject(status, "freememory", cJSON_CreateNumber(esp_get_free_heap_size()));
    cJSON_AddItemToObject(status, "uptime", cJSON_CreateString(uptime));
    cJSON_AddItemToObject(status, "curdate", cJSON_CreateString(curdate));
    cJSON_AddItemToObject(status, "devicename", cJSON_CreateString(hostname));
    cJSON_AddItemToObject(status, "version", cJSON_CreateString(version));
    
    *response = cJSON_Print(status);
    free(uptime);
    free(curdate);  
    free(version);
    cJSON_Delete(status);
    return ESP_OK;    
}

esp_err_t uiRouter(httpd_req_t *req) {
    char *uri = getClearURI(req->uri);
    char *response = NULL;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    char *content = NULL;
    
    if ((!strcmp(uri, "/ui/status")) && (req->method == HTTP_GET)) {
        httpd_resp_set_type(req, "application/json");
        // err = getStatus(&response);            
    } else if (!strcmp(uri, "/service/config/network")) {
        if (req->method == HTTP_GET) {
            httpd_resp_set_type(req, "application/json");
            err = getNetworkConfig(&response);
        } else if (req->method == HTTP_POST) {
            httpd_resp_set_type(req, "application/json");
            err = getContent(&content, req);
            if (err == ESP_OK) {
                err = setNetworkConfig(&response, content);    
            }
        }        
    } else if (!strcmp(uri, "/service/config/feed")) {
        if (req->method == HTTP_GET) {
            httpd_resp_set_type(req, "application/json");
            err = getFeedConfig(&response);
        } else if (req->method == HTTP_POST) {
            httpd_resp_set_type(req, "application/json");
            err = getContent(&content, req);
            if (err == ESP_OK) {
                err = setFeedConfig(&response, content);    
            }
        }            
    } else if ((!strcmp(uri, "/service/config/factoryReset")) && (req->method == HTTP_POST)) {
        if (getParamValue(req, "reset") != NULL) {
            err = setFactoryReset(&response);
        } else {
            err = ESP_FAIL;
            setErrorText(&response, "No reset");            
        }        
    } else if ((!strcmp(uri, "/service/reboot")) && (req->method == HTTP_POST)) {
        // TODO : create deffered task for reboot
        if (getParamValue(req, "reboot") != NULL) {
            ESP_LOGW(TAG, "Reebot!!!");
            //esp_restart();
            reboot = true;
            setTextJson(&response, "Reboot OK");
            err = ESP_OK;
        } else {
            err = ESP_FAIL;
            setErrorText(&response, "No reboot");            
        }        
    } else if ((!strcmp(uri, "/service/upgrade")) && (req->method == HTTP_POST)) {
        startOTA();        
        setTextJson(&response, "OTA OK");
        err = ESP_OK;        
    } else if ((!strcmp(uri, "/ui/deviceInfo")) && (req->method == HTTP_GET)) {
        httpd_resp_set_type(req, "application/json");
        err = getDeviceInfo(&response); 
    } else if ((!strcmp(uri, "/ui/feed")) && (req->method == HTTP_POST)) {
        httpd_resp_set_type(req, "application/json");        
        err = feed();
        setTextJson(&response, "Feed OK");
    }        
    // check result
    if (err == ESP_OK) {
        httpd_resp_set_status(req, "200");        
    } else if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "404");        
        setErrorText(&response, "Method not found!");        
        //httpd_resp_send(req, "Not found!"); //req->uri, strlen(req->uri));
    } else {
        httpd_resp_set_status(req, "400");
    }
    if (response != NULL) {
        httpd_resp_send(req, response, -1);
        free(response);
    }   
    if (content != NULL)
        free(content);
    free(uri);

    return ESP_OK;
}

void publish(uint8_t slaveId, uint8_t outputId, uint8_t action) {
    // MQTT
    ESP_LOGI(TAG, "MQTT publush slaveId %d, outputId %d, action %d", slaveId, outputId, action);
    // char* data = "OFF";
    // if (action == ACT_TOGGLE) {
    //     // узнать какое значение было до, чтобы понять что публиковать
    //     //ESP_LOGI(TAG, "MQTT current output value is %d", getOutputState(slaveId, outputId));
    //     if (getOutputState(slaveId, outputId) == 0)
    //         data = "ON";
    // } else if (action == ACT_ON) {
    //     data = "ON";
    // }
    
    // char topic[100];
    // char buf[5];
    // strcpy(topic, getNetworkConfigValueString("hostname"));
    // strcat(topic, "/");
    // itoa(slaveId, buf, 10);
    // strcat(topic, buf);
    // strcat(topic, "/");
    // itoa(outputId, buf, 10);
    // strcat(topic, buf);
    // strcat(topic, "\0");    
    // // if (getOutputState(slaveId, outputId) == 1)
    // //     data = "ON";
    // mqttPublish(topic, data);
}

SemaphoreHandle_t getSemaphore() {
    return sem_busy;
}

esp_err_t createSemaphore() {
    sem_busy = xSemaphoreCreateMutex();
    if (sem_busy == NULL)
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t feed() {
    if (!cJSON_IsObject(feedConfig)) {      
        ESP_LOGE(TAG, "feedConfig is not a json");
        return ESP_FAIL;
    }
    feedFlag = true; 
    
    return ESP_OK;
}

void stepperTask(void *pvParameter) {    
    uint16_t speed = 1000;
    uint16_t steps = 0;
    uint8_t stepFwd = 0;
    uint8_t stepRev = 0;

    ESP_LOGI(TAG, "Creating feed task");
    while(1)
    {
        if (feedFlag) {
            ESP_LOGI(TAG, "Feed in process...");   
            if (!cJSON_IsObject(feedConfig)) { 
                ESP_LOGE(TAG, "stepperTask. feedConfig is not a json");   
                feedFlag = false;
                continue;
            }

            if (cJSON_IsNumber(cJSON_GetObjectItem(feedConfig, "speed")))    
                speed = cJSON_GetObjectItem(feedConfig, "speed")->valueint;
            if (cJSON_IsNumber(cJSON_GetObjectItem(feedConfig, "stepCnt")))    
                steps = cJSON_GetObjectItem(feedConfig, "stepCnt")->valueint;
            if (cJSON_IsNumber(cJSON_GetObjectItem(feedConfig, "stepFwd")))    
                stepFwd = cJSON_GetObjectItem(feedConfig, "stepFwd")->valueint;
            if (cJSON_IsNumber(cJSON_GetObjectItem(feedConfig, "stepRev")))    
                stepRev = cJSON_GetObjectItem(feedConfig, "stepRev")->valueint;
            
            if (speed < 500) 
                speed = 500;

            gpio_set_level(PIN_EN, 0);
            rmt_item32_t items[1];
            items[0].level0 = 1;
            items[0].duration0 = speed;
            items[0].level1 = 0;
            items[0].duration1 = speed;
                        

            for (uint16_t i=0;i<steps;i++) {
                // fwd
                gpio_set_level(PIN_DIR, 1);
                for (int s=0;s<stepFwd;s++) {
                    rmt_write_items(RMT_TX_CHANNEL, items, 1, false);
                    ESP_ERROR_CHECK(rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY));
                }                
                // rev
                gpio_set_level(PIN_DIR, 0);
                for (int s=0;s<stepRev;s++) {
                    rmt_write_items(RMT_TX_CHANNEL, items, 1, false);
                    ESP_ERROR_CHECK(rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY));
                }                
            }            
            gpio_set_level(PIN_EN, 1);    
            ESP_LOGI(TAG, "Feed done");   
            feedFlag = false;
        }

        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

void initStepper() {
    ESP_LOGI(TAG, "Starting stepper motor control...");
    gpio_pad_select_gpio(PIN_EN);
    gpio_pad_select_gpio(PIN_DIR);
    gpio_pad_select_gpio(PIN_RESET);
    gpio_set_direction(PIN_RESET, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_EN, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_DIR, GPIO_MODE_OUTPUT);

    rmt_config_t rmt_tx;
    rmt_tx.rmt_mode = RMT_MODE_TX;
    rmt_tx.channel = RMT_TX_CHANNEL;
    rmt_tx.gpio_num = RMT_TX_GPIO_NUM;
    rmt_tx.mem_block_num = 1;
    rmt_tx.clk_div = 1;

    rmt_config(&rmt_tx);
    rmt_driver_install(rmt_tx.channel, 0, 0);

    gpio_set_level(PIN_RESET, 0);
    gpio_set_level(PIN_RESET, 1);
    gpio_set_level(PIN_EN, 1);

    xTaskCreate(&stepperTask, "stepperTask", 4096, NULL, 1, NULL);
    
    // xTaskCreatePinnedToCore(
    //             &stepperTask,   /* Function to implement the task */
    //             "stepperTask", /* Name of the task */
    //             4096,       /*Stack size in words */
    //             NULL,       /* Task input parameter */
    //             30,          /* Priority of the task */
    //             NULL,       /* Task handle. */
    //             1);  /* Core where the task should run */
}

void setFeedFlag(char *params) {
    uint16_t speed = 0, stepCnt = 0, stepFwd = 0, stepRev = 0;
    cJSON *jData = cJSON_Parse(params);
    if (cJSON_IsObject(jData)) {
        if (cJSON_IsNumber(cJSON_GetObjectItem(jData, "speed")))    
            speed = cJSON_GetObjectItem(jData, "speed")->valueint;
        if (cJSON_IsNumber(cJSON_GetObjectItem(jData, "stepCnt")))    
            stepCnt = cJSON_GetObjectItem(jData, "stepCnt")->valueint;
        if (cJSON_IsNumber(cJSON_GetObjectItem(jData, "stepFwd")))    
            stepFwd = cJSON_GetObjectItem(jData, "stepFwd")->valueint;
        if (cJSON_IsNumber(cJSON_GetObjectItem(jData, "stepRev")))    
            stepRev = cJSON_GetObjectItem(jData, "stepRev")->valueint;

        cJSON_ReplaceItemInObject(feedConfig, "speed", cJSON_CreateNumber(speed));        
        cJSON_ReplaceItemInObject(feedConfig, "stepCnt", cJSON_CreateNumber(stepCnt));        
        cJSON_ReplaceItemInObject(feedConfig, "stepFwd", cJSON_CreateNumber(stepFwd));        
        cJSON_ReplaceItemInObject(feedConfig, "stepRev", cJSON_CreateNumber(stepRev));        
        ESP_LOGI(TAG, "Feed. new values %d %d %d %d", speed, stepCnt, stepFwd, stepRev);
    }
    cJSON_Delete(jData);
    feedFlag = true;
}

void serviceTask(void *pvParameter) {
    ESP_LOGI(TAG, "Creating service task");
    uint32_t minMem = getNetworkConfigValueInt2("watchdog", "wdtmemsize");
    while(1)
    {   
        // every 1 second     
        if ((minMem > 0) && (esp_get_free_heap_size() < minMem)) {
            ESP_LOGE(TAG, "HEAP memory WDT triggered. Actual free memory is %d. Restarting...", esp_get_free_heap_size());
            esp_restart();
        }
        if (reboot) {
            static uint8_t cntReboot = 0;
            if (cntReboot++ >= 3) {
                ESP_LOGI(TAG, "Reboot now!");
                esp_restart();
            }
        }
        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

void initServiceTask() {
    xTaskCreate(&serviceTask, "serviceTask", 4096, NULL, 5, NULL);
}

// TODO : add 18b20 sensor and mqtt status