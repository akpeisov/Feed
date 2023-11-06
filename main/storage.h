//storage.h
#include "webServer.h"

esp_err_t initStorage();
esp_err_t loadTextFile(char * filename, char ** buffer);
esp_err_t saveTextFile(char * filename, char * buffer);
esp_err_t getFileWeb(httpd_req_t *req);
esp_err_t setFileWeb(httpd_req_t *req);
esp_err_t writeLog(char* type, char* buffer);
esp_err_t getLogFile(httpd_req_t *req);
esp_err_t listDirectory(int socket, const char *dirpath);
esp_err_t isDirExist(const char *dirpath);
esp_err_t getFile(int socket, const char *filepath);
esp_err_t setFile(int socket, const char *filepath);
esp_err_t deleteFile(const char *filepath);
esp_err_t makeDirectory(const char *dirpath);
esp_err_t removeDirectory(const char *filepath);