// storage file
// use internal storage, spiffs, sdcard
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_vfs_semihost.h"
#include "esp_vfs_fat.h"

#include "spiffs.h"
#include "webServer.h"
#include "utils.h"
#include "lwip/sockets.h"

//#define USE_SD
#define PATH_SIZE 100
#define BUF_SIZE 2048
#define MOUNT_POINT "/storage"
#define MAX_FILE_SIZE   (200*1024) // 200 KB
#define MAX_FILE_SIZE_STR "200KB"

static const char *TAG = "STORAGE";

uint16_t bootId;
char *logPath;

esp_err_t initNVS() {
	esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );
    return err;
}

esp_err_t storeNumber(const char *key, const uint16_t value) {
    nvs_handle my_handle;
    esp_err_t err;

    // Open
    err = nvs_open("config", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;
    ESP_LOGD(TAG, "Open ok");

    err = nvs_set_u16(my_handle, key, value);

    if (err != ESP_OK) return err;
    ESP_LOGD(TAG, "Store ok");

    // Commit
    err = nvs_commit(my_handle);
    if (err != ESP_OK) return err;

    ESP_LOGD(TAG, "Commit ok");
    // Close
    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t restoreNumber(const char *key, uint16_t *value) {
    nvs_handle my_handle;
    esp_err_t err;

    // Open
    err = nvs_open("config", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;
    ESP_LOGD(TAG, "Open ok");

    err = nvs_get_u16(my_handle, key, value);

    if (err != ESP_OK) return err;
    ESP_LOGD(TAG, "Store ok");

    // Commit
    err = nvs_commit(my_handle);
    if (err != ESP_OK) return err;

    ESP_LOGD(TAG, "Commit ok");
    // Close
    nvs_close(my_handle);
    return ESP_OK;
}

uint16_t getLastId() {
    uint16_t id = 0;
    restoreNumber("lastid", &id);
    id++;
    storeNumber("lastid", id);
    return id;    
}

esp_err_t initStorage() {
	initNVS();	
	if (initSPIFFS(MOUNT_POINT) != ESP_OK) {
		ESP_LOGE(TAG, "Can't init SPIFFS");
        return ESP_FAIL;
	}			
        
    char *buf = malloc(5);
    itoa(getLastId(), buf, 10);

    logPath = malloc(16+strlen(MOUNT_POINT));
    strcpy(logPath, MOUNT_POINT);    
    strcat(logPath, "/logs/");
    strcat(logPath, buf);
    strcat(logPath, ".txt");    
    free(buf);
    return ESP_OK;
}

esp_err_t loadTextFile(char * filename, char ** buffer) {    
	// аллокейтит буфер и считает весь файл в него
    char path[PATH_SIZE];
    strcpy(path, MOUNT_POINT);    
    strcat(path, filename);    
    ESP_LOGI(TAG, "Opening file path %s", path);
    FILE* f = fopen(path, "r");    
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s for reading", path);
        return ESP_FAIL;
    }

    fseek(f, 0L, SEEK_END);
    uint16_t bufSize = ftell(f);
    fseek(f, 0L, SEEK_SET);
    
    *buffer = (char*)malloc(bufSize+1);
    if (*buffer == NULL) {
        ESP_LOGE(TAG, "Can't allocate buffer for file %s", filename);
        return ESP_FAIL;
    } else {
        ESP_LOGD(TAG, "Allocate ok");
    }

    if (fread(*buffer, 1, bufSize, f) < 1) {
        ESP_LOGE(TAG, "Can't fread");
    }
    buffer[0][bufSize] = 0;    
    fclose(f);
    ESP_LOGD(TAG, "getTextFile finish");
    return ESP_OK;
}

esp_err_t saveTextFile(char * filename, char * buffer) {
    // save file to filesystem
    uint16_t bufSize = strlen(buffer);
    char path[PATH_SIZE];
    strcpy(path, MOUNT_POINT);    
    strcat(path, filename);    
    ESP_LOGI(TAG, "Saving file path %s", path);
    
    FILE *fd = fopen(path, "w");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to create file %s", path);        
        return ESP_FAIL;
    }
   
    if (bufSize != fwrite(buffer, 1, bufSize, fd)) {
        /* Couldn't write everything to file!
         * Storage may be full? */
        fclose(fd);
        unlink(path);
        ESP_LOGE(TAG, "File write failed!");
        return ESP_FAIL;
    }
        
    /* Close file upon upload completion */
    fclose(fd);
    ESP_LOGI(TAG, "File saved successfully");

    return ESP_OK;
}

esp_err_t getFileWebPath(httpd_req_t *req, char* path) {    
    ESP_LOGI(TAG, "Opening file path %s", path);
    
    FILE* f = fopen(path, "r");    
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s for reading", req->uri);
        return ESP_FAIL;
    }

    char *buffer = malloc(BUF_SIZE);    
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, path);
    size_t chunksize;
    do {
        /* Read file in chunks into the scratch buffer */
        //chunksize = fread(buffer, 1, SCRATCH_BUFSIZE, f);
        chunksize = fread(buffer, 1, BUF_SIZE, f);

        if (chunksize > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, buffer, chunksize) != ESP_OK) {
                fclose(f);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                free(buffer);
                return ESP_FAIL;
            }
        }

        /* Keep looping till the whole file is sent */
    } while (chunksize != 0);
    
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    free(buffer);
    return ESP_OK;
}

esp_err_t getFileWeb(httpd_req_t *req) {    
    char path[PATH_SIZE];
    strcpy(path, getWWWroot());    
    if (req->uri[strlen(req->uri) - 1] == '/') 
        strcat(path, "/index.html");
    else   
        strcat(path, req->uri);    
    return getFileWebPath(req, path);
}    

esp_err_t getLogFile(httpd_req_t *req) {    
    return getFileWebPath(req, logPath);
}

// esp_err_t getFileWeb(httpd_req_t *req) {
//     char path[PATH_SIZE];
//     strcpy(path, getWWWroot());    
//     if (req->uri[strlen(req->uri) - 1] == '/') 
//         strcat(path, "/index.html");
//     else   
//     	strcat(path, req->uri);    
//     ESP_LOGI(TAG, "Opening file path %s", path);
    
//     FILE* f = fopen(path, "r");    
//     if (f == NULL) {
//         ESP_LOGE(TAG, "Failed to open file %s for reading", req->uri);
//         return ESP_FAIL;
//     }

//     char *buffer = malloc(BUF_SIZE);    
//     if (buffer == NULL) {
//         ESP_LOGE(TAG, "Failed to allocate buffer");
//         return ESP_FAIL;
//     }

//     set_content_type_from_file(req, path);
//     size_t chunksize;
//     do {
//         /* Read file in chunks into the scratch buffer */
//         //chunksize = fread(buffer, 1, SCRATCH_BUFSIZE, f);
//         chunksize = fread(buffer, 1, BUF_SIZE, f);

//         if (chunksize > 0) {
//             /* Send the buffer contents as HTTP response chunk */
//             if (httpd_resp_send_chunk(req, buffer, chunksize) != ESP_OK) {
//                 fclose(f);
//                 ESP_LOGE(TAG, "File sending failed!");
//                 /* Abort sending file */
//                 httpd_resp_sendstr_chunk(req, NULL);
//                 /* Respond with 500 Internal Server Error */
//                 httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
//                 free(buffer);
//                 return ESP_FAIL;
//             }
//         }

//         /* Keep looping till the whole file is sent */
//     } while (chunksize != 0);
    
//     fclose(f);
//     httpd_resp_send_chunk(req, NULL, 0);
//     free(buffer);
//     return ESP_OK;
// }

esp_err_t setFileWeb(httpd_req_t *req) {
    char filepath[PATH_SIZE];
    FILE *fd = NULL;
    struct stat file_stat;
    strcpy(filepath, req->uri + sizeof("/service/upload"));
    
    /* Filename cannot have a trailing '/' */
    if (filepath[strlen(filepath) - 1] == '/') {
        ESP_LOGE(TAG, "Invalid filepath : %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filepath");
        return ESP_FAIL;
    }

    if (stat(filepath, &file_stat) == 0) {
        ESP_LOGE(TAG, "File already exists : %s", filepath);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File already exists");
        return ESP_FAIL;
    }

    /* File cannot be larger than a limit */
    if (req->content_len > MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "File size must be less than "
                            MAX_FILE_SIZE_STR "!");
        /* Return failure to close underlying connection else the
         * incoming file content will keep the socket busy */
        return ESP_FAIL;
    }

    char fileFullPath[PATH_SIZE];    
    strcpy(fileFullPath, getWWWroot()); 
    //strcpy(fileFullPath, sdmount);
    strcat(fileFullPath, "/");
    strcat(fileFullPath, filepath);

    fd = fopen(fileFullPath, "w");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to create file : %s", fileFullPath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_FAIL;
    }

    /* Retrieve the pointer to scratch buffer for temporary storage */
    //char *buf = ((struct file_server_data *)req->user_ctx)->scratch;
    char *buf = malloc(BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Cannot malloc buf!");
        return ESP_FAIL;
    }
    int received;

    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = req->content_len;

    while (remaining > 0) {

        ESP_LOGI(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buf, MIN(remaining, BUF_SIZE))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }

            /* In case of unrecoverable error,
             * close and delete the unfinished file*/
            fclose(fd);
            unlink(filepath);

            ESP_LOGE(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            free(buf);
            return ESP_FAIL;
        }

        /* Write buffer content to file on storage */
        if (received && (received != fwrite(buf, 1, received, fd))) {
            /* Couldn't write everything to file!
             * Storage may be full? */
            fclose(fd);
            unlink(filepath);

            ESP_LOGE(TAG, "File write failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage");
            free(buf);
            return ESP_FAIL;
        }

        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;
    }

    /* Close file upon upload completion */
    fclose(fd);
    ESP_LOGI(TAG, "File reception complete");

    /* Redirect onto root to see the updated file list */
    // httpd_resp_set_status(req, "303 See Other");
    // httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "File uploaded successfully");
    free(buf);
    return ESP_OK;
}

esp_err_t writeLog(char* type, char* buffer) {
    // write logs
    //temporary off
    return ESP_OK;
    FILE *fd = fopen(logPath, "a+");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to open file %s", logPath);        
        return ESP_FAIL;
    }

    char buf[10];
    char *time = getCurrentDateTime("%Y-%m-%d %H:%M:%S");    
    fwrite(time, 1, strlen(time), fd);
    free(time);
    fwrite(";", 1, 1, fd);
    unsigned long ticks = esp_timer_get_time() / 1000;
    itoa(ticks, buf, 10);
    fwrite(buf, 1, strlen(buf), fd);

    fwrite(";", 1, 1, fd);
    fwrite(type, 1, strlen(type), fd);
    fwrite(";", 1, 1, fd);

    int bufSize = strlen(buffer);    
    if (bufSize != fwrite(buffer, 1, bufSize, fd)) {
        /* Couldn't write everything to file!
         * Storage may be full? */
        fclose(fd);
        unlink(logPath);
        ESP_LOGE(TAG, "File write failed!");
        return ESP_FAIL;
    }
    fwrite("\n", 1, 1, fd);    
    fclose(fd);
    ESP_LOGD(TAG, "File saved successfully");
    return ESP_OK;
}

esp_err_t makeDirectory(const char *dirpath) {
    char path[PATH_SIZE];
    bzero(path, PATH_SIZE);
    strcpy(path, MOUNT_POINT);    
    strcat(path, dirpath);
    int mk_ret = mkdir(path, 0775);    
    ESP_LOGI(TAG, "Creating directory %s", path);
    if (mk_ret != 0)
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t listDirectory(int socket, const char *dirpath) {    
    // (c) colhoze zarya-vostok
    ESP_LOGI(TAG, "Dir list path is %s", dirpath);    
    char path[PATH_SIZE];
    bzero(path, PATH_SIZE);
    strcpy(path, MOUNT_POINT);    
    strcat(path, dirpath);    
    
    char entrypath[255];
    char entrysize[16];
    const char *entrytype;
    char entryname[128];
    struct stat entry_stat;
    struct dirent *entry;
    char str[255];

    char *lastdirs[10];
    uint8_t lastdirsQty = 0;
    // 
    
    size_t dirpath_len = strlen(path);
    DIR *dir = opendir(path);

    if (!dir) {
        ESP_LOGE(TAG, "Failed to stat dir %s", path);        
        return ESP_FAIL;
    }

    if (path[strlen(path)-1] != '/') {
        dirpath_len++;
        strcat(path, "/");
    }
    strlcpy(entrypath, path, sizeof(entrypath));
    // ESP_LOGI(TAG, "entrypath %s", entrypath);
    
    /* Iterate over all files / folders and fetch their names and sizes */
    while ((entry = readdir(dir)) != NULL) {
        //stat(entrypath, &entry_stat);         
        if (strstr(entry->d_name, "/")) {
            entrytype = "d";
            strlcpy(entryname, entry->d_name, strchr(entry->d_name, '/')-entry->d_name+1);
            sprintf(entrysize, "%d", 0);            
            // file contains / (for spiffs it's normal)
            // TODO : check if it directory already printed skip it
            bool exist = false;
            for (uint8_t i=0; i<lastdirsQty; i++) {
                // ESP_LOGW(TAG, "lastdirs %s", lastdirs[i]);
                if (!strcmp(lastdirs[i], entryname)) {
                    // already exists
                    exist = true;
                    break;
                }
            } 
            if (!exist) {                                
                lastdirs[lastdirsQty] = malloc(strlen(entryname)+1);
                //strcpy(lastdirs[lastdirsQty++], entry->d_name); 
                strcpy(lastdirs[lastdirsQty++], entryname); 
            } else {
                continue;
            }
        } else { 
            entrytype = (entry->d_type == DT_DIR ? "d" : "-");        
            strlcpy(entrypath + dirpath_len, entry->d_name, sizeof(entrypath) - dirpath_len);
            if (stat(entrypath, &entry_stat) == -1) {
                ESP_LOGE(TAG, "Failed to stat %s. Fullpath %s", entry->d_name, entrypath);
                continue;
            }
            sprintf(entrysize, "%8ld", entry_stat.st_size);
            strcpy(entryname, entry->d_name);
        }

        strcpy(str, entrytype);
        strcat(str, "rw-rw-rw-   1 root     root     ");
        
        strcat(str, entrysize);
        strcat(str, " Jan 01 12:00 ");
        strcat(str, entryname);
        strcat(str, "\n");
        // sprintf(str, "%srw-rw-rw-   1 root     root     %8ld Aug 25 12:25 %s",
        //         entrytype, entry_stat.st_size, entry->d_name);
        send(socket, str, strlen(str), 0);
        //-rw-rw-rw-   1 root     root           33 Aug 25 12:25 ddns.php?host=old2.elim.kz&myip=178.88.21.181        
        //-rw-rw----   1 root     root           31 Jul  7 18:03 ddns.php?host=old2.elim.kz&myip=5.251.211.5
        //drwxrwx---   1 root     root         2048 Jan  1 06:00 skins
    }
    closedir(dir);
    for (uint8_t i=0; i<lastdirsQty; i++) {
        free(lastdirs[i]);
    }
    return ESP_OK;
}

esp_err_t getFile(int socket, const char *filepath) {    
    char path[PATH_SIZE];
    bzero(path, PATH_SIZE);
    strcpy(path, MOUNT_POINT);    
    strcat(path, filepath);    
    ESP_LOGI(TAG, "Opening file %s", path);
    FILE* f = fopen(path, "r");    
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s for reading", filepath);
        return ESP_FAIL;
    }

    char *buffer = malloc(BUF_SIZE);    
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return ESP_FAIL;
    }
    
    size_t chunksize;
    do {
        /* Read file in chunks into the scratch buffer */
        //chunksize = fread(buffer, 1, SCRATCH_BUFSIZE, f);
        chunksize = fread(buffer, 1, BUF_SIZE, f);

        if (chunksize > 0) {
            if (send(socket, buffer, chunksize, 0) < 0) {       
                fclose(f);
                ESP_LOGE(TAG, "File sending failed!");                
                free(buffer);
                return ESP_FAIL;
            }
        }

        /* Keep looping till the whole file is sent */
    } while (chunksize != 0);
    
    fclose(f);    
    free(buffer);
    return ESP_OK;
}

esp_err_t setFile(int socket, const char *filepath) {    
    char path[PATH_SIZE];
    bzero(path, PATH_SIZE);
    strcpy(path, MOUNT_POINT);    
    strcat(path, filepath);    
    ESP_LOGI(TAG, "Creating file %s", path);
    FILE* f = fopen(path, "w");    
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s for writing", filepath);
        return ESP_FAIL;
    }

    char *buffer = malloc(BUF_SIZE);    
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return ESP_FAIL;
    }

    uint16_t len;    
    do {
        len = recv(socket, buffer, BUF_SIZE, 0);
        if (len > 0)
            if (len != fwrite(buffer, 1, len, f)) {
                ESP_LOGE(TAG, "Failed to receive buffer");
                fclose(f);    
                free(buffer);
                return ESP_FAIL;
            }
    } while (len > 0);

    fclose(f);    
    free(buffer);
    return ESP_OK;
}

esp_err_t deleteFile(const char *filepath) {    
    char path[PATH_SIZE];
    bzero(path, PATH_SIZE);
    strcpy(path, MOUNT_POINT);    
    strcat(path, filepath);    
    ESP_LOGI(TAG, "Deleting file %s", path);
    if (unlink(path) == 0)
        return ESP_OK;
    return ESP_FAIL;
}

esp_err_t removeDirectory(const char *filepath) {    
    char path[PATH_SIZE];
    bzero(path, PATH_SIZE);
    strcpy(path, MOUNT_POINT);    
    strcat(path, filepath);    
    ESP_LOGI(TAG, "Removing directory %s", path);
    if (rmdir(path) == 0)
        return ESP_OK;
    return ESP_FAIL;
}


esp_err_t isDirExist(const char *dirpath) {
    // change directory for ftp
    // return ESP_OK if directory exists
    char path[PATH_SIZE];
    strcpy(path, MOUNT_POINT);    
    strcat(path, dirpath);    
    DIR *dir = opendir(path);
    
    if (!dir) {
        ESP_LOGE(TAG, "Failed to stat dir %s", dirpath);
        return ESP_FAIL;
    }
    closedir(dir);
    return ESP_OK;
}