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
#include "ftp.h"

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

uint32_t getOwnAddr() {
    return ownAddr;
}

void networkEvent(bool ready) {
    if (ready) {
        ESP_LOGW(TAG, "Network up");
        if (!networkInited) {
            networkInited = true;
            // run once
            sntpInit();            
            initMQTT();
            // initScheduler();
        }
        if (getNetworkConfigValueBool2("rlog", "enabled")) {
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
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");        
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
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
    // ESP_LOGI(TAG, "address %zu, 0x%08x", ownAddr, ownAddr);
    // ESP_LOGW(TAG, "a1 %d, %d, %d, %d", (ownAddr>>24)&0xFF, (ownAddr>>16)&0xFF, (ownAddr>>8)&0xFF, (ownAddr)&0xFF );
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
    // gpio_pad_select_gpio(0);
    // gpio_set_direction(0, GPIO_MODE_OUTPUT);    
    // gpio_set_level(0, 0);
    // esp_rom_delay_us(200); // insert min input assert time
    // gpio_set_level(0, 1);

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
    //phy_config.reset_gpio_num = getNetworkConfigValueInt2("eth", "resetGPIO"); // TODO : uncomment and fix
    phy_config.reset_gpio_num = -1;
    phy_config.reset_timeout_ms = 500;
    mac_config.smi_mdc_gpio_num = 23; //CONFIG_EXAMPLE_ETH_MDC_GPIO;
    mac_config.smi_mdio_gpio_num = 18; //CONFIG_EXAMPLE_ETH_MDIO_GPIO;
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
        // vTaskDelay(3000 / portTICK_RATE_MS); // TODO : what is this???
        esp_netif_set_ip_info(eth_netif, &ip_info);
        esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    ESP_LOGI(TAG, "Starting eth");
    //ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    return esp_eth_start(eth_handle);
}

static void sta_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        networkEvent(false);    
        esp_wifi_connect();        
        ESP_LOGI(TAG, "trying to connect to the AP...");
        // if (s_retry_num < 10) {
        //     esp_wifi_connect();
        //     s_retry_num++;
        //     ESP_LOGI(TAG, "retry to connect to the AP");
        // } else {
        //     xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        // }
        // ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        // s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "WIFI Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "WIFI IP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "WIFI MASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "WIFI GW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ownAddr = ip_info->ip.addr;    
    networkReady = true;
    networkEvent(true);
    initFTP();    
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

void wifi_init_sta(void) {
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

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = ""
        },
    };
    strcpy((char *)wifi_config.sta.ssid, getNetworkConfigValueString2("wifi", "ssid"));
    strcpy((char *)wifi_config.sta.password, getNetworkConfigValueString2("wifi", "pass"));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_sta finished. Waiting for WiFi connect to %s", getNetworkConfigValueString2("wifi", "ssid"));    
}

void wifi_init_softap(void) {
     esp_netif_t* wifiAP = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ip_info;
    esp_netif_ip_info_t ipInfo;
    IP4_ADDR(&ipInfo.ip, 172,25,52,1);
    IP4_ADDR(&ipInfo.gw, 172,25,52,1);
    IP4_ADDR(&ipInfo.netmask, 255,255,255,0);
    esp_netif_dhcps_stop(wifiAP);
    esp_netif_set_ip_info(wifiAP, &ipInfo);
    esp_netif_dhcps_start(wifiAP);

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
    char str[100];
    sprintf(str, "SNTP current time is %s", strftime_buf);    
}

static void obtain_time(void) {
    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    //int retry = 0;
    //const int retry_count = 10;
    // while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
    //     ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    //     vTaskDelay(2000 / portTICK_PERIOD_MS);
    // }
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
    // #ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    //     sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    // #endif
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
    //ESP_LOGI(TAG, "After init net and loop event. Free heap size is %d", esp_get_free_heap_size());

    if (isEthEnabled()) {
        initEth();    
        ESP_LOGI(TAG, "After init eth. Free heap size is %d", esp_get_free_heap_size());
    }
    // 
    if (isWifiEnabled()) {       
        wifi_init_sta();
        ESP_LOGI(TAG, "After init wifi. Free heap size is %d", esp_get_free_heap_size());
    }    
    
    // если сеть не настроена, включаем точку доступа
    if (!isEthEnabled() && !isWifiEnabled()) {
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