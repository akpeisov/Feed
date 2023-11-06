// ftp.c

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "storage.h"
#include "utils.h"
#include "network.h"
#include "core.h"

#define PORT 21

static const char *TAG = "FTP";

static char curdir[128] = "/";
static xQueueHandle xDataQueue = NULL;
static xQueueHandle xDataQueue2 = NULL;
static char *userName = "admin";
static char *userPass = "admin";

static void ftpPasvSocket(void *pvParameters) {
    char addr_str[128];
    int port = (int)pvParameters;
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    struct sockaddr_in6 dest_addr;
    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(port);
    ip_protocol = IPPROTO_IP;

    int listen_sock = socket(AF_INET, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGD(TAG, "FTP pasv socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGD(TAG, "FTP pasv socket bound, port %d", port);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }
    
    ESP_LOGD(TAG, "FTP socket listening");

    struct sockaddr_in6 source_addr; // Large enough for both IPv4 or IPv6
    uint addr_len = sizeof(source_addr);
    int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
        //break;
    }

    // Convert ip address to string
    if (source_addr.sin6_family == PF_INET) {
        inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
    } else if (source_addr.sin6_family == PF_INET6) {
        inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
    }
    ESP_LOGD(TAG, "Socket accepted ip address: %s. Waiting for data", addr_str);

    char *data;        
    xQueueReceive(xDataQueue, &data, portMAX_DELAY);
    char filepath[255];
    ESP_LOGD(TAG, "xQueueReceive %s", data);
    if (strstr(data, "LIST")) {
        // list files
        listDirectory(sock, curdir);
    } else if (strstr(data, "RETR")) {
        // transfer file
        // get file name
        bzero(filepath, sizeof(filepath));
        strcpy(filepath, curdir);
        if (filepath[strlen(filepath)-1] != '/')
            strcat(filepath, "/");
        strcat(filepath, data+5);
        rtrim(filepath, " ");
        if (getFile(sock, filepath) != ESP_OK) {
            strcpy(data, "FAIL");                
        } else {
            strcpy(data, "OK");
        }
    } else if (strstr(data, "STOR")) {
        // receive file
        // get file name
        bzero(filepath, sizeof(filepath));
        strcpy(filepath, curdir);
        if (filepath[strlen(filepath)-1] != '/')
            strcat(filepath, "/");
        strcat(filepath, data+5);
        rtrim(filepath, " ");
        if (setFile(sock, filepath) != ESP_OK) {
            strcpy(data, "FAIL");                
        } else {
            strcpy(data, "OK");
        }
    }
    
    if (xQueueSend(xDataQueue2, &data, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Can't send xqueue2!");
    }
    
    ESP_LOGD(TAG, "Passive socket closing");

    shutdown(sock, 0);
    close(sock);        
    
CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
    ESP_LOGD(TAG, "Closing listen ftpPasvSocket");
}

static void do_retransmit(const int sock)
{
    int len;
    char* rx_buffer = malloc(128);
    char* hello = "220 ESP server\n";
    send(sock, hello, strlen(hello), 0);
    char answer[100];
    char* cmd;
    char* args;
    bool userok = false;
    bool connOk = false;
    char tmpbuf[128];
    strcpy(curdir, "/");
    do {
        len = recv(sock, rx_buffer, 128, 0);
        if (len < 0) {
            ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
        } else if (len == 0) {
            ESP_LOGD(TAG, "Connection closed");
        } else {
            strcpy(answer, "500 command not recognized");
            ESP_LOG_BUFFER_HEXDUMP("rx_buffer", rx_buffer, len, CONFIG_LOG_DEFAULT_LEVEL);
            rx_buffer[len] = 0; // Null-terminate whatever is received and treat it like a string
            
            if (rx_buffer[len-1] == '\n')
                rx_buffer[len-1] = 32;
            if (rx_buffer[len-1] == 0x0A)
                rx_buffer[len-1] = 32;
            if (rx_buffer[len-2] == 0x0D)
                rx_buffer[len-2] = 32;

            //ESP_LOGI(TAG, "< %s", rx_buffer);

            args = strstr(rx_buffer, " ")+1;
            rtrim(args, " ");
            cmd = strtok(rx_buffer, " ");
            if (cmd == NULL) {
                cmd = "EMPTY";
            }
            bzero(tmpbuf, sizeof(tmpbuf));
            //ESP_LOGI(TAG, "args !%s!", args);

            if (strstr(cmd, "USER")) {
                if (!strcmp(args, userName)) {
                    userok = true;
                    strcpy(answer, "331 password required");                
                } else {
                    strcpy(answer, "430 Invalid username");
                }                
            } else if (strstr(cmd, "PASS")) {
                if (userok && !strcmp(args, userPass)) {
                    strcpy(answer, "230 user logged in");
                } else {
                    strcpy(answer, "430 Invalid username");                   
                } 
            } else if (strstr(cmd, "QUIT")) {
                strcpy(answer, "221 Good bye\n");
                send(sock, answer, strlen(answer), 0);
                break;
            } else if (strstr(cmd, "SYST")) {
                strcpy(answer, "215 UNIX Type: ESP");
            } else if (strstr(cmd, "TYPE")) {
                if (strstr(args, "I")) {
                    strcpy(answer, "200 Type set to I");
                } else {
                    strcpy(answer, "200 Type set to A");
                }
            } else if (strstr(cmd, "PASV")) { // enter passive mode
                //ESP_LOGI(TAG, "address %lu", getOwnAddr());
                sprintf(answer, "227 Entering Passive Mode (%d,%d,%d,%d,100,5)", 
                        (getOwnAddr())&0xFF, 
                        (getOwnAddr()>>8)&0xFF, 
                        (getOwnAddr()>>16)&0xFF, 
                        (getOwnAddr()>>24)&0xFF);
                //strcpy(answer, "227 Entering Passive Mode (192,168,99,48,100,5)"); // upper 8 bits, lower 8 bits, so 100*256+5 = 25605
                xTaskCreate(ftpPasvSocket, "ftp_pasv_server", 4096, (void*)25605, 5, NULL);
                connOk = true;
            } else if (strstr(cmd, "LIST")) { // ls
                if (!connOk) { // if connection exists
                    strcpy(answer, "425 Can't build data connection");                    
                } else {
                    strcpy(answer, "150 Opening connection\n");
                    if (xQueueSend(xDataQueue, &cmd, portMAX_DELAY) != pdTRUE) {
                        ESP_LOGE(TAG, "Can't send xqueue!");
                    }
                    send(sock, answer, strlen(answer), 0);    
                    //strcpy(cmd, "NNN");
                    //xQueueReceive(xDataQueue2, &cmd, portMAX_DELAY);
                    if (xQueueReceive(xDataQueue2, &cmd, pdMS_TO_TICKS(60000)) == pdTRUE) {
                        strcpy(answer, "226 Transfer Complete");     
                    } else {
                        strcpy(answer, "451 Transfer error");
                    }
                    connOk = false;
                }
            } else if (strstr(cmd, "PWD")) { // print work dir
                strcpy(answer, "257 \"");
                strcat(answer, curdir);
                strcat(answer, "\"");
            } else if (strstr(cmd, "CWD")) {  // change work dir               
                if (!strcmp(args, "..")) {
                    // get parent dir
                    if (!strcmp(curdir, "/")) {
                        // already root dir
                        strcpy(tmpbuf, "/");
                    } else {
                        // get parent dir
                        uint8_t n = strlen(curdir) - 1;
                        // get clean path
                        while ((curdir[n] == '/') && (n>0))
                            n--;
                        while (n > 0) {
                            if (curdir[n] == '/' ) {
                                break;
                            }
                            if (n==0)
                                break;
                            n--;
                        }
                        if (n>0) {
                            strncpy(tmpbuf, curdir, n);
                            if ((n > 1) && (tmpbuf[n-1] == '/'))
                                tmpbuf[n-1] = 0;
                        } else
                            strcpy(tmpbuf, "/");                          
                        //ESP_LOG_BUFFER_HEXDUMP("tmpbuf", tmpbuf, 30, CONFIG_LOG_DEFAULT_LEVEL);
                    }
                } else if(!strcmp(args, "/")) {
                    strcpy(tmpbuf, "/");
                    tmpbuf[1] = 0;
                } else if (args[0] == '/') {
                    // provide path like /tmp
                    strcpy(tmpbuf, args);
                    // if path /tmp/ remove last /
                    if ((strlen(tmpbuf) > 2) && (tmpbuf[strlen(tmpbuf)-1] == '/'))
                        tmpbuf[strlen(tmpbuf)-1] = 0;
                } else {
                    // change dir
                    strcpy(tmpbuf, curdir);                    
                    if (tmpbuf[strlen(tmpbuf)-1] != '/') {
                        strcat(tmpbuf, "/");                    
                    }
                    strcat(tmpbuf, args);
                }
                if (isDirExist(tmpbuf) == ESP_OK) {
                    bzero(curdir, sizeof(curdir));
                    strcpy(curdir, tmpbuf);
                    strcpy(answer, "250 CWD command successful");
                } else {
                    strcpy(answer, "501 Can't change directory");
                }
            } else if (strstr(cmd, "RETR")) { // receive file
                if (!connOk) {
                    strcpy(answer, "425 Can't build data connection");                    
                } else {
                    if (xQueueSend(xDataQueue, &rx_buffer, portMAX_DELAY) != pdTRUE) {
                        ESP_LOGE(TAG, "Can't send xqueue!");
                    }
                    strcpy(answer, "150 Opening connection\n");
                    send(sock, answer, strlen(answer), 0);    
                    
                    if (xQueueReceive(xDataQueue2, &cmd, pdMS_TO_TICKS(120000)) == pdTRUE) {
                        strcpy(answer, "226 Transfer Complete");     
                    } else {
                        strcpy(answer, "451 Transfer error");
                    }
                    connOk = false;
                }
            } else if (strstr(cmd, "STOR")) { // store file
                if (!connOk) {
                    strcpy(answer, "425 Can't build data connection");                    
                } else {
                    if (xQueueSend(xDataQueue, &rx_buffer, portMAX_DELAY) != pdTRUE) {
                        ESP_LOGE(TAG, "Can't send xqueue!");
                    }
                    strcpy(answer, "150 Opening connection\n");
                    send(sock, answer, strlen(answer), 0);    
                    
                    if (xQueueReceive(xDataQueue2, &cmd, pdMS_TO_TICKS(120000)) == pdTRUE) {
                        strcpy(answer, "226 Transfer Complete");     
                    } else {
                        strcpy(answer, "451 Transfer error");
                    }
                    connOk = false;
                }
            } else if (strstr(cmd, "DELE")) { // delete file
                if (deleteFile(args) == ESP_OK) {
                    strcpy(answer, "250 File deleted");
                } else {
                    strcpy(answer, "450 Can't delete file");
                }                
            } else if (strstr(cmd, "RMD")) { // remove directory
                strcpy(tmpbuf, curdir);
                if (tmpbuf[strlen(tmpbuf)-1] != '/') {
                    strcat(tmpbuf, "/");
                }
                strcat(tmpbuf, args);
                if (removeDirectory(tmpbuf) == ESP_OK) {
                    strcpy(answer, "250 Directory deleted");
                } else {
                    strcpy(answer, "450 Can't delete directory");
                }                
            } else if (strstr(cmd, "MKD")) { // make directory
                // SPIFFS make directory not supported 
                strcpy(answer, "257 Directory created");
                // TODO : make directory for SD mode, or create special file for SPIFFS FTP
                //tmpbuf curdir
                strcpy(tmpbuf, curdir);
                if (tmpbuf[strlen(tmpbuf)-1] != '/') {
                    strcat(tmpbuf, "/");
                }
                strcat(tmpbuf, args);
                makeDirectory(tmpbuf);
            }

            ESP_LOGD(TAG, "> %s", answer);
            strcat(answer, "\n");
            send(sock, answer, strlen(answer), 0);
            //ESP_LOG_BUFFER_HEXDUMP(">", answer, 30, CONFIG_LOG_DEFAULT_LEVEL);
            //ESP_LOGI(TAG, "Curdir %s", curdir);            
        }
    } while (len > 0);
    free(rx_buffer);
}

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    struct sockaddr_in6 dest_addr;
    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(PORT);
    ip_protocol = IPPROTO_IP;

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGD(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGD(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {

        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_in6 source_addr; // Large enough for both IPv4 or IPv6
        uint addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Convert ip address to string
        if (source_addr.sin6_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
        } else if (source_addr.sin6_family == PF_INET6) {
            inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
        }
        ESP_LOGD(TAG, "Socket accepted ip address: %s", addr_str);

        do_retransmit(sock);

        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

void initFTP(void)
{
    if (!getNetworkConfigValueBool2("ftp", "enabled")) {
        ESP_LOGI(TAG, "No need to init FTP");
        return;
    } else {
        userName = getNetworkConfigValueString2("ftp", "user");
        userPass = getNetworkConfigValueString2("ftp", "pass");
    }   
    xDataQueue = xQueueCreate(1, 10);
    xDataQueue2 = xQueueCreate(1, 10);
    xTaskCreate(tcp_server_task, "ftp_server", 4096, (void*)AF_INET, 5, NULL);
}
