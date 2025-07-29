//network.c
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "core.h"
#include "esp_event.h"
#include <esp_wifi.h>
#include "lwip/ip_addr.h"
#include "freertos/event_groups.h"
#include <string.h>
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "udp_logging.h"
#include "mqtt.h"
#include "esp_smartconfig.h"

#define AP_SSID "HomeIO"
#define AP_PASS "12345678"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "NETWORK";
static esp_eth_handle_t eth_handle = NULL;
bool sntpStarted = false;
bool networkReady = false;
static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *netif = NULL;
uint32_t ownAddr;
void sntpInit();
bool networkInited = false;
wifi_config_t wifi_config;
char ethIP[16] = "0.0.0.0", wifiIP[16] = "0.0.0.0";

uint32_t getOwnAddr() {
    return ownAddr;
}

char *getETHIPStr() {
    char *res = malloc(16);    
    strcpy(res, ethIP);    
    return res;
}

char *getWIFIIPStr() {
    char *res = malloc(16);    
    strcpy(res, wifiIP);
    return res;
}

char *getIPStr() {
    char *res = malloc(40);

    if (ethIP[0] != '0') {
        strcat(res, "ETH ");
        strcat(res, ethIP);
    }

    if ((ethIP[0] != '0') && (wifiIP[0] != '0')) {
        strcat(res, " ");
    }

    if (wifiIP[0] != '0') {      
        strcat(res, "WIFI ");
        strcat(res, wifiIP);
    }

    if ((ethIP[0] == '0') && (wifiIP[0] == '0')) {
        strcpy(res, "No network");
    }

    return res;
}

void networkEvent(bool ready) {
    if (ready) {
        ESP_LOGW(TAG, "Network up");
        if (!networkInited) {
            networkInited = true;
            // run once
            sntpInit();            
            initMQTT();
            initScheduler();
        }
        if (getNetworkConfigValueBool2("rlog", "enabled")) {
            ESP_LOGI(TAG, "Running rlog");
            udp_logging_init(getNetworkConfigValueString2("rlog", "server"),
                             getNetworkConfigValueInt2("rlog", "port"), 
                             udp_logging_vprintf);
        }
    } else {
        ESP_LOGE(TAG, "Network down");
    }
}

/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle2 = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            esp_eth_ioctl(eth_handle2, ETH_CMD_G_MAC_ADDR, mac_addr);
            ESP_LOGI(TAG, "Ethernet Link Up");
            ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                     mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            networkEvent(false);
            ethIP[0] = '-';
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");        
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            ethIP[0] = '-';
            break;
        default:
            ESP_LOGW(TAG, "Unknown event_id %d from ethernet", event_id);
            break;
    }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ownAddr = ip_info->ip.addr;    
    sprintf(ethIP, IPSTR, IP2STR(&ip_info->ip));
    networkReady = true;
    networkEvent(true);    
}

bool isEthEnabled() {
    return getNetworkConfigValueBool2("eth", "enabled");
}
bool isWifiEnabled() {    
    return getNetworkConfigValueBool2("wifi", "enabled");
}

esp_err_t initEth() {
    static esp_netif_t *eth_netif;
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&cfg);   
    netif = eth_netif; 
    // Set default handlers to process TCP/IP stuffs
    ESP_ERROR_CHECK(esp_eth_set_default_handlers(eth_netif));
    ESP_ERROR_CHECK(esp_netif_set_hostname(eth_netif, getNetworkConfigValueString("hostname")));
    // Register user defined event handers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    //phy_config.reset_gpio_num = 0;//CONFIG_EXAMPLE_ETH_PHY_RST_GPIO;
    // сделал для Миши, т.к. не грузится контроллер с етх, почему-то иногда впадает в режим загрузки.
    // ножку резета отрезаю и перекидываю на gpio14 (SD_CLK). 
    // если нет значения в ethResetGpio то вернет 0, что как раз соответствует остальным платам.
    phy_config.reset_gpio_num = getNetworkConfigValueInt2("eth", "resetGPIO");
    ESP_LOGI(TAG, "resetGPIO %d", getNetworkConfigValueInt2("eth", "resetGPIO"));
    // phy_config.reset_timeout_ms = 500;
    mac_config.smi_mdc_gpio_num = 23; 
    mac_config.smi_mdio_gpio_num = 18;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan8720(&phy_config);
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    //esp_eth_handle_t eth_handle = NULL;            
    //ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));
    if (esp_eth_driver_install(&config, &eth_handle) != ESP_OK)
        return ESP_FAIL;
        /* attach Ethernet driver to TCP/IP stack */
    //ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
    if (esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)) != ESP_OK) 
        return ESP_FAIL;        
    
    if (!getNetworkConfigValueBool2("eth", "dhcp")) {
        // static ip    
        ESP_LOGI(TAG, "Set static IP %s", getNetworkConfigValueString2("eth", "ip"));
        esp_netif_ip_info_t ip_info;
        ipaddr_aton(getNetworkConfigValueString2("eth", "ip"), (ip_addr_t *)&ip_info.ip);
        ipaddr_aton(getNetworkConfigValueString2("eth", "netmask"), (ip_addr_t *)&ip_info.netmask);
        ipaddr_aton(getNetworkConfigValueString2("eth", "gateway"), (ip_addr_t *)&ip_info.gw);
        esp_netif_dns_info_t dns;
        //IP4_ADDR(&dns.ip.u_addr.ip4, 8, 8, 8, 8);     
        ipaddr_aton(getNetworkConfigValueString("dns"), (ip_addr_t *)&dns.ip.u_addr.ip4);
        
        esp_netif_dhcp_status_t status;
        esp_netif_dhcpc_get_status(eth_netif, &status);
        if (status != ESP_NETIF_DHCP_STOPPED) {
            ESP_LOGI(TAG, "Stopping dhcp client");
            ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));
        }
        esp_netif_set_ip_info(eth_netif, &ip_info);
        esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    ESP_LOGI(TAG, "Starting eth");
    return esp_eth_start(eth_handle);
}

static void sta_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED. Trying to connect to the AP %s with password %s", wifi_config.sta.ssid, wifi_config.sta.password);        
        wifiIP[0] = '-';
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));        
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
    }
}

static void wifi_got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "WIFI Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "WIFI IP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "WIFI MASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "WIFI GW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ownAddr = ip_info->ip.addr;  
    sprintf(wifiIP, IPSTR, IP2STR(&ip_info->ip));  
    networkReady = true;
    networkEvent(true);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

esp_err_t wifi_init_sta(void) {
    if (getNetworkConfigValueString2("wifi", "ssid") == NULL) {
        ESP_LOGE(TAG, "No SSID present. Changing to AP mode");
        return ESP_FAIL;
    }
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    netif = sta_netif;
    esp_netif_set_hostname(sta_netif, getNetworkConfigValueString("hostname"));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (!getNetworkConfigValueBool2("wifi", "dhcp")) {
        // static    
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));
        esp_netif_ip_info_t ip_info;
        // IP4_ADDR(&ip_info.ip, 192, 168, 99, 19);
        // IP4_ADDR(&ip_info.gw, 192, 168, 99, 98);
        // IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        ipaddr_aton(getNetworkConfigValueString2("wifi", "ip"), (ip_addr_t *)&ip_info.ip);
        ipaddr_aton(getNetworkConfigValueString2("wifi", "netmask"), (ip_addr_t *)&ip_info.netmask);
        ipaddr_aton(getNetworkConfigValueString2("wifi", "gateway"), (ip_addr_t *)&ip_info.gw);
        esp_netif_set_ip_info(sta_netif, &ip_info);

        esp_netif_dns_info_t dns;
        //IP4_ADDR(&dns.ip.u_addr.ip4, 8, 8, 8, 8);
        ipaddr_aton(getNetworkConfigValueString("dns"), (ip_addr_t *)&dns.ip.u_addr.ip4);
        esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_got_ip_event_handler, NULL));

    strcpy((char *)wifi_config.sta.ssid, getNetworkConfigValueString2("wifi", "ssid"));
    strcpy((char *)wifi_config.sta.password, getNetworkConfigValueString2("wifi", "pass"));

    // strcpy((char *)wifi_config.sta.ssid, "Alana");
    // strcpy((char *)wifi_config.sta.password, "zxcv1234");

    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    // wifi_config.sta.threshold.rssi = -127;
    // wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // ESP_LOGI(TAG, "wifi_init_sta finished. Waiting for WiFi connect to %s", getNetworkConfigValueString2("wifi", "ssid"));    
    return ESP_OK;
}

void wifi_init_softap(void) {
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 6,
            .password = AP_PASS,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };    

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_ap finished. AP %s with password %s", AP_SSID, AP_PASS);    
}

void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Notification of a time synchronization event");
    time_t now;
    struct tm timeinfo;
    time(&now);
    char strftime_buf[64];    
    setenv("TZ", getNetworkConfigValueString("ntpTZ"), 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);
    //char str[100];
    //sprintf(str, "SNTP current time is %s", strftime_buf);    
}

static void obtain_time(void) {
    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
}

void sntpInit() {
    if (sntpStarted)
        return;
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    //sntp_setservername(0, "pool.ntp.org");    
    ESP_LOGI(TAG, "NTP server is %s", getNetworkConfigValueString("ntpserver"));
    sntp_setservername(0, getNetworkConfigValueString("ntpserver"));
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    sntp_init();

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2020 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Getting time over NTP.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }    
    sntpStarted = true;
}

esp_err_t initNetwork() {
    // Initialize TCP/IP network interface (should be called only once in application)
    ESP_ERROR_CHECK(esp_netif_init());        
    // Create default event loop that running in background
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    bool wifiInited = false;
    
    if (isEthEnabled()) {
        //phyPower(false);
        initEth();    
        ESP_LOGI(TAG, "After init eth. Free heap size is %d", esp_get_free_heap_size());
    }
    // 
    if (isWifiEnabled()) {       
        if (wifi_init_sta() == ESP_OK)            
            wifiInited = true;
        ESP_LOGI(TAG, "After init wifi. Free heap size is %d", esp_get_free_heap_size());
    }    
    
    // если сеть не настроена, включаем точку доступа
    if (!isEthEnabled() && !isWifiEnabled() && !wifiInited) {
        wifi_init_softap();        
    }
    return ESP_OK;
}

esp_netif_t *getNetif(void)
{
    return netif;
}

bool isNetworkReady() {
    return networkReady;
}

int8_t getRSSI() {           
    wifi_ap_record_t wifidata;
    esp_wifi_sta_get_ap_info(&wifidata);
    if (wifidata.primary != 0) {
        return wifidata.rssi;
    }
    return 127;
}