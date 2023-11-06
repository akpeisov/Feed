#include <string.h>
#include <fcntl.h>
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"
#include "storage.h"
#include "core.h"
//#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "WEB";

#define MAXCONTENTSIZE 32768

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
// #define SCRATCH_BUFSIZE (10240)

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)
char * wwwroot;

esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename) {
    if (IS_FILE_EXT(filename, ".pdf")) {
        return httpd_resp_set_type(req, "application/pdf");
    } else if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".jpeg")) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");    
    } else if (IS_FILE_EXT(filename, ".js")) {        
        httpd_resp_set_hdr(req, "Date", "Fri, 07 Feb 2020 16:07:54 GMT");
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=2628000, public");
        return httpd_resp_set_type(req, "application/javascript");
    } else if (IS_FILE_EXT(filename, ".css")) {
        return httpd_resp_set_type(req, "text/css");
    } else if (IS_FILE_EXT(filename, ".png")) {
        return httpd_resp_set_type(req, "image/png");    
    } else if (IS_FILE_EXT(filename, ".json")) {
        return httpd_resp_set_type(req, "application/json; charset=utf-8");        
    } else {
        return httpd_resp_set_type(req, "text/plain");
    }
    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

char* getClearURI(const char * uri) {
    char *newuri; 
    const char *delim_pos = strchr(uri, '?');
    if (delim_pos != NULL) {
        newuri = malloc(delim_pos - uri + 1);    
        memcpy(newuri, uri, delim_pos - uri);
        newuri[delim_pos - uri] = 0;
    } else {
        newuri = malloc(strlen(uri)+1);
        memcpy(newuri, uri, strlen(uri)+1);
    }
    ESP_LOGD(TAG, "URI %s", newuri);
    return newuri;
}

char* getParamValue(httpd_req_t *req, char *paramName) {    
    char*  buf;
    size_t buf_len;
    char *value = calloc(sizeof(char), 32);
    if (value == NULL) {
        ESP_LOGE(TAG, "Can't calloc value");
        return NULL;
    }
    
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Can't malloc buf");            
            return NULL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGD(TAG, "Found URL query => %s", buf);            
            if (httpd_query_key_value(buf, paramName, value, 32) == ESP_OK) {
                ESP_LOGD(TAG, "Found URL query parameter => %s", value);
            }            
        }
        free(buf);
    }
    return value;
}

esp_err_t toDecimal(char *src, uint8_t *val) {
    char *end;    
    *val = strtol(src, &end, 10);
    esp_err_t err = ESP_OK;
    if (src == end) {        
        err = ESP_FAIL;
    }
    free(src);
    return err;
}

esp_err_t getContent(char **dst, httpd_req_t *req) {
    if (req->content_len > MAXCONTENTSIZE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
        return ESP_FAIL;
    }
    *dst = (char*)malloc(req->content_len+1);
    if (*dst == NULL) {
        ESP_LOGE(TAG, "Can't allocate content buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Can't allocate content buffer");
        return ESP_FAIL;
    }
    int cur_len = 0;
    int received = 0;
    while (cur_len < req->content_len) {
        received = httpd_req_recv(req, *dst + cur_len, req->content_len);
        if (received <= 0) {            
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get content");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    //*dst[req->content_len] = '\0';
    return ESP_OK;    
}

static esp_err_t file_get_handler(httpd_req_t *req) {
    return getFileWeb(req);
}

static esp_err_t ui_handler(httpd_req_t *req) {
    //ESP_LOGI(TAG, "Test ui %s", req->uri);
    SemaphoreHandle_t sem = getSemaphore();
    if (xSemaphoreTake(sem, portMAX_DELAY) == pdTRUE) {
        uiRouter(req);
        xSemaphoreGive(sem);
    }
    return ESP_OK;
}

// static esp_err_t ui_handler(httpd_req_t *req) {
//     uiRouter(req);
//     return ESP_OK;
// }

static esp_err_t http_router(httpd_req_t *req) {
    // main http router
    if (!strncmp(req->uri, "/service/upload/", 16) && req->method == HTTP_POST) {
        return setFileWeb(req);
    } 
    if (!strncmp(req->uri, "/service/", 9)) {
        return ui_handler(req);
    }  
    if (!strncmp(req->uri, "/ui/", 4)) {
        return ui_handler(req);
    }  
    if (!strncmp(req->uri, "/v1.0", 5)) {
        return ui_handler(req);
    }  
    if (!strncmp(req->uri, "/alice/", 7)) {
        return ui_handler(req);
    }  
    
    return file_get_handler(req);
}

esp_err_t startWebserver()
{
    //REST_CHECK(base_path, "wrong base path", err);
    // rest_server_context_t *rest_context = calloc(1, sizeof(rest_server_context_t));
    // REST_CHECK(rest_context, "No memory for rest context", err);
    // strlcpy(rest_context->wwwroot, wwwroot, sizeof(rest_context->wwwroot));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.core_id = 0;

    ESP_LOGI(TAG, "Starting HTTP Server");
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Start server failed");
        return ESP_FAIL;
    }
    
    httpd_uri_t get_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = http_router
    };    
    httpd_register_uri_handler(server, &get_uri);
    
    httpd_uri_t post_uri = {
        .uri = "/*",
        .method = HTTP_POST,
        .handler = http_router
    };    
    httpd_register_uri_handler(server, &post_uri);

    httpd_uri_t head_uri = {
        .uri = "/*",
        .method = HTTP_HEAD,
        .handler = http_router
    };    
    httpd_register_uri_handler(server, &head_uri);


    // httpd_uri_t file_service_post = {
    //     .uri       = "/service/upload/*",  
    //     .method    = HTTP_POST,
    //     .handler   = setFileWeb
    // };    
    // httpd_register_uri_handler(server, &file_service_post);
        
    // httpd_uri_t file_service_config_get = {
    //     .uri       = "/service/config/*",  
    //     .method    = HTTP_GET,
    //     .handler   = ui_handler
    // };
    // httpd_register_uri_handler(server, &file_service_config_get);

    // httpd_uri_t file_service_config_post = {
    //     .uri       = "/service/*",  
    //     .method    = HTTP_POST,
    //     .handler   = ui_handler
    // };
    // httpd_register_uri_handler(server, &file_service_config_post);

    // httpd_uri_t ui_get_uri = {
    //     .uri = "/ui/*",
    //     .method = HTTP_GET,
    //     .handler = ui_handler
    // };
    // httpd_register_uri_handler(server, &ui_get_uri);

    // httpd_uri_t ui_post_uri = {
    //     .uri = "/ui/*",
    //     .method = HTTP_POST,
    //     .handler = ui_handler
    // };
    // httpd_register_uri_handler(server, &ui_post_uri);

    // /* URI handler for getting web server files */
    // httpd_uri_t file_get_uri = {
    //     .uri = "/*",
    //     .method = HTTP_GET,
    //     .handler = file_get_handler
    // };    
    // httpd_register_uri_handler(server, &file_get_uri);

    ESP_LOGI(TAG, "WebServer started");
    return ESP_OK;
}

char* getWWWroot() {
    return wwwroot;
}

esp_err_t initWebServer() {
    wwwroot = calloc(1, 30);
    strcpy(wwwroot, "/storage/web");
    return startWebserver();
}