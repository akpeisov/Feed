//core.h
#include "webServer.h"
#include "freertos/semphr.h"

esp_err_t loadConfig();

uint16_t getNetworkConfigValueInt(const char* name);
uint16_t getNetworkConfigValueInt2(const char* parentName, const char* name);
bool getNetworkConfigValueBool(const char* name);
bool getNetworkConfigValueBool2(const char* parentName, const char* name);
char *getNetworkConfigValueString(const char* name);
char *getNetworkConfigValueString2(const char* parentName, const char* name);

esp_err_t uiRouter(httpd_req_t *req);
bool isReboot();

SemaphoreHandle_t getSemaphore();
esp_err_t createSemaphore();

void initStepper();
void setFeedFlag(char *params);
void initServiceTask();