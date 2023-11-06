//webServer.h
#include "esp_http_server.h"

// typedef struct rest_server_context {
// //    char base_path[ESP_VFS_PATH_MAX + 1];
//     char wwwroot[30];
//     //char scratch[SCRATCH_BUFSIZE];
// } rest_server_context_t;

char *getWWWroot();
esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename);
esp_err_t initWebServer();
char *getClearURI(const char *uri);
char *getParamValue(httpd_req_t *req, char *paramName);
esp_err_t toDecimal(char *src, uint8_t *val);
esp_err_t getContent(char **dst, httpd_req_t *req);