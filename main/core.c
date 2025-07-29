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
#include "math.h"
#include "esp_sntp.h"
#include "driver/adc.h"
#include "network.h"

static const char *TAG = "CORE";
static cJSON *networkConfig;
static cJSON *jTemperatures;
static cJSON *jScheduler;

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

#define WS_1    13 //2
#define WS_2    33
#define WS_3    32
#define OW      14
#define ADC     36

#define  setbit(var, bit)    ((var) |= (1 << (bit)))
#define  clrbit(var, bit)    ((var) &= ~(1 << (bit)))

bool reboot = false;
SemaphoreHandle_t sem_busy = NULL;
void processScheduler();

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
    cJSON_AddItemToObject(networkConfig, "otaurl", cJSON_CreateString("https://api.akpeisov.kz/water.bin"));
    
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

esp_err_t saveTemperatures() {
    char *data = cJSON_Print(jTemperatures);    
    esp_err_t err = saveTextFile("/config/temperatures.json", data);
    free(data);
    return err;
}

esp_err_t loadTemperatures() {
    char * buffer;
    if (loadTextFile("/config/temperatures.json", &buffer) == ESP_OK) {
        cJSON *parent = cJSON_Parse(buffer);
        if (!cJSON_IsArray(parent)) {
            free(buffer);
            return ESP_FAIL;
        }
        cJSON_Delete(jTemperatures);
        jTemperatures = parent;        
        free(buffer);
    } else {
        ESP_LOGI(TAG, "can't read temperatures config");
        cJSON_Delete(jTemperatures);
        jTemperatures = cJSON_CreateArray();       
        saveTemperatures();
    }    
    return ESP_OK;
}

esp_err_t saveScheduler() {
    char *data = cJSON_Print(jScheduler);    
    esp_err_t err = saveTextFile("/config/scheduler.json", data);
    free(data);
    return err;
}

esp_err_t loadScheduler() {
    char * buffer;
    if (loadTextFile("/config/scheduler.json", &buffer) == ESP_OK) {
        cJSON *parent = cJSON_Parse(buffer);
        if (!cJSON_IsArray(parent)) {
            free(buffer);
            return ESP_FAIL;
        }
        cJSON_Delete(jScheduler);
        jScheduler = parent;        
        free(buffer);
    } else {
        ESP_LOGI(TAG, "can't read scheduler config");
        cJSON_Delete(jScheduler);
        jScheduler = cJSON_CreateArray();       
        saveScheduler();
    }    
    return ESP_OK;
}

esp_err_t loadConfig() {
    if ((loadNetworkConfig() == ESP_OK) && 
        (loadTemperatures() == ESP_OK) &&
        (loadScheduler() == ESP_OK)) {
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
    char *ethip = getETHIPStr();
    char *wifiip = getWIFIIPStr();
    cJSON_AddItemToObject(status, "freememory", cJSON_CreateNumber(esp_get_free_heap_size()));
    cJSON_AddItemToObject(status, "uptime", cJSON_CreateString(uptime));
    cJSON_AddItemToObject(status, "curdate", cJSON_CreateString(curdate));
    cJSON_AddItemToObject(status, "devicename", cJSON_CreateString(hostname));
    cJSON_AddItemToObject(status, "version", cJSON_CreateString(version));
    cJSON_AddItemToObject(status, "rssi", cJSON_CreateNumber(getRSSI()));
    cJSON_AddItemToObject(status, "ethip", cJSON_CreateString(ethip));
    cJSON_AddItemToObject(status, "wifiip", cJSON_CreateString(wifiip));        
    *response = cJSON_Print(status);
    free(uptime);
    free(curdate);  
    free(version);
    free(ethip);
    free(wifiip);
    cJSON_Delete(status);
    return ESP_OK;    
}

esp_err_t getTemperatures(char **response) {    
    *response = cJSON_Print(jTemperatures);
    return ESP_OK;    
}

esp_err_t setTemperatures(char **response, char *content) {
    cJSON *parent = cJSON_Parse(content);
    
    if(!cJSON_IsArray(parent)) {
        setErrorTextJson(response, "Is not a JSON array");    
        cJSON_Delete(parent);
        return ESP_FAIL;
    }       
    cJSON_Delete(jTemperatures);
    jTemperatures = parent;    
    saveTemperatures();
    setTextJson(response, "OK");    
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
//        err = feed();
        setTextJson(&response, "Feed OK");    
    } else if (!strcmp(uri, "/service/config/temperatures")) {
        httpd_resp_set_type(req, "application/json");
        if (req->method == HTTP_GET) {            
            err = getTemperatures(&response);
        } else if (req->method == HTTP_POST) {            
            err = getContent(&content, req);
            if (err == ESP_OK) {
                err = setTemperatures(&response, content);    
            }
        }   
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

SemaphoreHandle_t getSemaphore() {
    return sem_busy;
}

esp_err_t createSemaphore() {
    sem_busy = xSemaphoreCreateMutex();
    if (sem_busy == NULL)
        return ESP_FAIL;
    return ESP_OK;
}

void serviceTask(void *pvParameter) {
    ESP_LOGI(TAG, "Creating service task");
    uint32_t minMem = getNetworkConfigValueInt2("watchdog", "wdtmemsize");
    uint8_t cnt=0;
    while(1)
    {   
        // every 1 minute
        if (cnt++>=60) {
            cnt = 0;
            ESP_LOGI(TAG, "Actual free memory is %d", esp_get_free_heap_size());            
            processScheduler();
        }
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

double roundF(float x) {
    double x10 = 10.0 * x;
    return (round(x10)/10.0);
}

void setTemperature(char* adr, float value) {  
    char *date = getCurrentDateTime("%Y-%m-%dT%H:%M:%SZ");  
    bool found = false;
    float tmpValue = value;
    char *tmpName = NULL;
    cJSON *childTemp = jTemperatures->child;
    while (childTemp) {
        if (cJSON_IsString(cJSON_GetObjectItem(childTemp, "address")) &&
            !strcmp(cJSON_GetObjectItem(childTemp, "address")->valuestring, adr)) {
            if (cJSON_IsNumber(cJSON_GetObjectItem(childTemp, "value"))) {
                tmpValue = cJSON_GetObjectItem(childTemp, "address")->valuedouble;
                tmpName = cJSON_GetObjectItem(childTemp, "name")->valuestring;
                cJSON_ReplaceItemInObject(childTemp, "value", cJSON_CreateNumber(roundF(value)));                
            } else {
                cJSON_AddItemToObject(childTemp, "value", cJSON_CreateNumber(roundF(value)));
            }
            if (cJSON_IsString(cJSON_GetObjectItem(childTemp, "date")))
                cJSON_ReplaceItemInObject(childTemp, "date", cJSON_CreateString(date));
            else
                cJSON_AddItemToObject(childTemp, "date", cJSON_CreateString(date));
            found = true;
            break;                
        }
        childTemp = childTemp->next;
    }
    if (!found) {
        // add new item
        cJSON *newItem = cJSON_CreateObject();
        cJSON_AddItemToObject(newItem, "date", cJSON_CreateString(date));
        cJSON_AddItemToObject(newItem, "value", cJSON_CreateNumber(roundF(value)));
        cJSON_AddItemToObject(newItem, "address", cJSON_CreateString(adr));
        cJSON_AddItemToObject(newItem, "name", cJSON_CreateString(adr));
        cJSON_AddItemToArray(jTemperatures, newItem);
        //tmpName = adr;
    }
    free(date);

    // if changed publish
    if (tmpValue != value && tmpName != NULL) {
        char topic[100];
        strcpy(topic, getNetworkConfigValueString("hostname"));
        strcat(topic, "/temperature/");
        strcat(topic, tmpName);
        mqttPublishF(topic, value);        
    }
}

void mqttScheduler(uint16_t curTime) {
    // Раз в минуту отправлять статус в MQTT?
    // static uint16_t lastSchedulerTime = 0;
    if (!getNetworkConfigValueBool2("mqtt", "enabled")) {
        return;
    }
    // general info publish
    char topic[100];
    char* info;
    esp_err_t err = getDeviceInfo(&info);    
    if (err == ESP_OK) {
        strcpy(topic, getNetworkConfigValueString("hostname"));
        strcat(topic, "/info\0");
        mqttPublish(topic, info);
        free(info);
    }
    // temperatures publish
    char *temp = cJSON_PrintUnformatted(jTemperatures);        
    strcpy(topic, getNetworkConfigValueString("hostname"));
    strcat(topic, "/temperatures");
    mqttPublish(topic, temp);
    free(temp);
}

void initScheduler() {
    ESP_LOGI(TAG, "Initiating scheduler");
    // сбрасываем у всех задач признак выполнения
    cJSON *childTask = jScheduler->child;
    while (childTask) {       
        if (cJSON_IsBool(cJSON_GetObjectItem(childTask, "done")))
            cJSON_ReplaceItemInObject(childTask, "done", cJSON_CreateFalse());
        else
            cJSON_AddItemToObject(childTask, "done", cJSON_CreateFalse());        
        if (!cJSON_IsString(cJSON_GetObjectItem(childTask, "name"))) {
            cJSON_AddItemToObject(childTask, "name", cJSON_CreateString("Noname task"));        
        }
        if (!cJSON_IsBool(cJSON_GetObjectItem(childTask, "enabled")))            
            cJSON_AddItemToObject(childTask, "enabled", cJSON_CreateFalse());        
        childTask = childTask->next;    
    }
}

void processScheduler() {
    // run minutely    
    time_t rawtime;
    struct tm *info;
    time(&rawtime);
    info = localtime(&rawtime);
    uint16_t currentTime = info->tm_hour*60 + info->tm_min;
    uint16_t grace = 0; // период, в который еще можно запустить задачу
    static uint16_t lastSchedulerTime = 0;
    ESP_LOGI(TAG, "Scheduler time %d. Day of week %d", currentTime, info->tm_wday);
    if (currentTime < lastSchedulerTime) {        
        // когда перевалит за 0.00
        initScheduler();
    }
    lastSchedulerTime = currentTime;
    // currentTime - minutes from 0.00
    // пройти все задачи, время которых еще не настало отметить как done = false
    // время которых прошло или наступило проверить на грейс период, по умолчанию он 5 мин
    // если задача со статусом done = false - выполнить ее и пометить как выполненная
    cJSON *childTask = jScheduler->child;
    while (childTask) {       
        ESP_LOGD(TAG, "Scheduler task %s done is %d", 
                 cJSON_GetObjectItem(childTask, "name")->valuestring, cJSON_IsTrue(cJSON_GetObjectItem(childTask, "done")));
        if (cJSON_IsTrue(cJSON_GetObjectItem(childTask, "enabled")) &&
            cJSON_IsNumber(cJSON_GetObjectItem(childTask, "time")) &&
            (currentTime >= cJSON_GetObjectItem(childTask, "time")->valueint) &&
            !cJSON_IsTrue(cJSON_GetObjectItem(childTask, "done"))) {
            ESP_LOGD(TAG, "Scheduler inside loop");
            // если время настало
            // не будет работать в 0.00
            if (cJSON_IsNumber(cJSON_GetObjectItem(childTask, "grace")))
                grace = cJSON_GetObjectItem(childTask, "grace")->valueint;
            else
                grace = 1; // default grace time
            ESP_LOGD(TAG, "Scheduler task %s, time %d, grace %d",
                     cJSON_GetObjectItem(childTask, "name")->valuestring,
                     cJSON_GetObjectItem(childTask, "time")->valueint,
                     grace);
            // day of week
            if (cJSON_IsArray(cJSON_GetObjectItem(childTask, "dow"))) {
                // есть дни недели
                ESP_LOGD(TAG, "dow exists");
                bool taskDow = false;
                cJSON *iterator = NULL;    
                cJSON_ArrayForEach(iterator, cJSON_GetObjectItem(childTask, "dow")) {
                    if (cJSON_IsNumber(iterator)) {
                        if (iterator->valueint == info->tm_wday) {
                            taskDow = true;
                            break;
                        }                        
                    }       
                } 
                if (!taskDow) {
                    ESP_LOGD(TAG, "task %s dow not today", cJSON_GetObjectItem(childTask, "name")->valuestring);
                    childTask = childTask->next; 
                    continue;
                }
                ESP_LOGD(TAG, "task %s dow today, processing task...", cJSON_GetObjectItem(childTask, "name")->valuestring);
            }
            if (currentTime - cJSON_GetObjectItem(childTask, "time")->valueint <= grace) {
                // выполнить задачу
                // получить действия
                ESP_LOGD(TAG, "Scheduler. Task %s. Processing actions...",
                         cJSON_GetObjectItem(childTask, "name")->valuestring);
                
                cJSON_ReplaceItemInObject(childTask, "done", cJSON_CreateTrue());                
            }            
        }
        childTask = childTask->next;    
    }
    mqttScheduler(currentTime);
}

void initIOasInput(uint8_t gpio) {
    gpio_pad_select_gpio(gpio);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);    
}

void ADCTask(void *pvParameter) {
    // Инициализировать АЦП с использованием функции adc1_config_width() для установки разрядности АЦП
    // и функции adc1_config_channel_atten() для настройки канала и аттенюатора:
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_0);
    
    uint16_t oldValue = 0;
    uint8_t delta = getNetworkConfigValueInt2("adc", "delta");
    uint16_t minPressure = getNetworkConfigValueInt2("adc", "min");
    uint16_t maxPressure = getNetworkConfigValueInt2("adc", "max");
    uint16_t period = getNetworkConfigValueInt2("adc", "period");
    if (period == 0) 
        period = 5000;
    char topic[100];
    ESP_LOGI(TAG, "ADC delta %d, min %d, max %d", delta, minPressure, maxPressure);
    bool pl = false, ph = false;
    while (1) {
        uint32_t adc_value = adc1_get_raw(ADC1_CHANNEL_0);
        printf("ADC Value: %d\n", adc_value);

        if (abs(adc_value - oldValue) > delta) {
            oldValue = adc_value;            
            strcpy(topic, getNetworkConfigValueString("hostname"));
            strcat(topic, "/pressure");
            mqttPublishF(topic, adc_value);            
        }

        if (adc_value < minPressure) {
            if (!pl) {
                strcpy(topic, getNetworkConfigValueString("hostname"));
                strcat(topic, "/pressureText");
                mqttPublish(topic, "low");
                pl = true;
            }
        } else {
            pl = false;
        }

        if (adc_value > maxPressure) {
            if (!ph) {
                strcpy(topic, getNetworkConfigValueString("hostname"));
                strcat(topic, "/pressureText");
                mqttPublish(topic, "high");
                ph = true;
            }
        } else {
            ph = false;
        }

        vTaskDelay(period * 1000 / portTICK_RATE_MS);

/*

    #define DEFAULT_VREF    1100        //Use adc2_vref_to_gpio() to obtain a better estimate
    #define NO_OF_SAMPLES   64          //Multisampling

        uint32_t adc_reading = 0;
        //Multisampling
        for (int i = 0; i < NO_OF_SAMPLES; i++) {
            if (unit == ADC_UNIT_1) {
                adc_reading += adc1_get_raw((adc1_channel_t)channel);
            }
        }
        adc_reading /= NO_OF_SAMPLES;
        //Convert adc_reading to voltage in mV
        uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
        printf("Raw: %d\tVoltage: %dmV Ph%f\n", adc_reading, voltage, calcPh(voltage));
        vTaskDelay(pdMS_TO_TICKS(1000));
        */
    }
}
    
void initADC() {
    xTaskCreate(&ADCTask, "ADCTask", 4096, NULL, 5, NULL);
}    

void waterTask(void *pvParameter) {
/*
    1 0 0 0
    1 1 0 0
    1 1 1 0
    -------
    7 3 1 0    

    empty 7
    low 3
    med 1
    full 0
*/
    initIOasInput(WS_1);
    initIOasInput(WS_2);
    initIOasInput(WS_3);

    uint8_t oldValue = 0;

    while (1) {
        uint8_t value = gpio_get_level(WS_1);
        value |= gpio_get_level(WS_2) << 1;
        value |= gpio_get_level(WS_3) << 2;
        
        if (oldValue != value) {
            oldValue = value;
            char topic[100];
            char *cValue;
            ESP_LOGI(TAG, "water new value %d", value);
            switch (value) {
                case 0:
                    cValue = "Full";
                    break; 
                case 1:
                    cValue = "Half";
                    break; 
                case 3:
                    cValue = "Low";
                    break; 
                case 7:
                    cValue = "Empty";
                    break;    
                default:
                    cValue = "Error";
                    break;             
            }
            strcpy(topic, getNetworkConfigValueString("hostname"));
            strcat(topic, "/water");
            mqttPublish(topic, cValue);
            //free(cValue);
        }
        vTaskDelay(10000 / portTICK_RATE_MS);
    }
    
}

void initWater() {
    xTaskCreate(&waterTask, "waterTask", 4096, NULL, 5, NULL);
}  