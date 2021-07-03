/**
 * UDP Broadcast Server
 */
#include <stdint.h>

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi_types.h"

#include "esp_event.h"
#include "esp_netif.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "esp_wifi_default.h"
#include "esp_wifi.h"

size_t g_udp_sendto_count = 0;
size_t g_udp_sendto_error_count = 0;
size_t g_udp_recv_count = 0;
size_t g_udp_recv_error_count = 0;

static const char *TAG = "BCAST_S";

static TaskHandle_t h_udp_server_task = NULL;
static esp_netif_t *w_ap_netif = NULL;

#define APP_UDP_SENDTO_ERR_DELAY (50)
#define CONFIG_APP_UDP_SERVER_PORT (3333)
#define CONFIG_APP_WIFI_CHANNEL_SELECT (11)
#define CONFIG_HOSTAPP_WIFI_SSID "CSI_BCAST"
#define CONFIG_HOSTAPP_WIFI_PASSWORD "CSI_BCAST_PASSWORD123"

static uint32_t wifi_get_local_ip(void)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(w_ap_netif, &ip_info);
    return (ip_info.ip.addr);
}

static esp_timer_handle_t h_periodic_timer[0];

void periodic_timer_callback(void *arg)
{
    ESP_LOGI(TAG, "#Sent: %zu, #Err: %zu per sec. Free heap %d B", g_udp_sendto_count, g_udp_sendto_error_count, esp_get_free_heap_size());
    g_udp_sendto_count = 0; g_udp_sendto_error_count = 0;
}

void setup_periodic_timer(void (*periodic_timer_callback)(void*), uint32_t period_us) {
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = periodic_timer_callback,
        /* name is optional, but may help identify the timer when debugging */
        .name = "periodic"
    };

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &h_periodic_timer[0]));
    /* The timer has been created but is not running yet */

    /* Start the timers */
    ESP_ERROR_CHECK(esp_timer_start_periodic(h_periodic_timer[0], period_us));
}


/*
 * Setting up a UDP server
 */
void udp_broadcast_server_task(void *pParams)
{
#define TX_BUF_SZ (128)
    char tx_buffer[TX_BUF_SZ];
    int addr_family;
    int ip_protocol;
    bool keep_alive = true;
    int64_t timestamp_us = 0;
    int err = 0;
    struct sockaddr_in source_r_bcast_addr;
    bzero(&source_r_bcast_addr, sizeof(struct sockaddr_in));
    int rx_buf_len = 0;

    struct sockaddr_in local_addr;
    bzero(&local_addr, sizeof(struct sockaddr_in));
    local_addr.sin_addr.s_addr = wifi_get_local_ip();
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(CONFIG_APP_UDP_SERVER_PORT);
    addr_family = AF_INET;
    ip_protocol = IPPROTO_IP;

    setup_periodic_timer(&periodic_timer_callback, (1 * 1000000));
    
    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
    }

    int opt_val = 1;
    err = setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt_val, sizeof(opt_val));
    if (err < 0)
    {
        ESP_LOGE(TAG, "Failed to set braodcast property. Error %d - %s", errno, strerror(errno));
        close(sock);
        vTaskDelete(NULL);
    }

    err = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));
    if (err < 0)
    {
        ESP_LOGE(TAG, "Failed to set reuseaddr property. Error %d - %s", errno, strerror(errno));
        close(sock);
        vTaskDelete(NULL);
    }

    // set destination broadcast addresses for sending from these sockets
    source_r_bcast_addr.sin_family = AF_INET;                                        // PF_INET;
    source_r_bcast_addr.sin_port = htons(CONFIG_APP_UDP_SERVER_PORT);                //  remote port
    source_r_bcast_addr.sin_addr.s_addr = inet_addr(CONFIG_APP_BROADCAST_IPV4_ADDR); // remote IP address
    ESP_LOGI(TAG, "Broadcast enabled. Sending UDP with port number %u", CONFIG_APP_UDP_SERVER_PORT);

    err = bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr));
    if (err < 0)
    {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d - %s", errno, strerror(errno));
        shutdown(sock, 0);
        close(sock);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "Socket bound to port %d", CONFIG_APP_UDP_SERVER_PORT);

    while (keep_alive)
    {
        timestamp_us = esp_timer_get_time();
        rx_buf_len = snprintf(&tx_buffer[0], TX_BUF_SZ, "cnt %zu, ts %" PRId64, g_udp_sendto_count, timestamp_us);
        // ESP_LOGI(TAG, "timestamp_us %s, rx_buf_len %d", tx_buffer, rx_buf_len);

        int err = sendto(sock, tx_buffer, rx_buf_len, 0, (struct sockaddr *)&source_r_bcast_addr, sizeof(source_r_bcast_addr));
        if (err < 0)
        {
            ++g_udp_sendto_error_count;
            // ESP_LOGE(TAG, "Error sending. sleep: errno %d - %s", errno, strerror(errno));
            vTaskDelay(APP_UDP_SENDTO_ERR_DELAY / portTICK_PERIOD_MS);
        }
        else
        {
            ++g_udp_sendto_count;
        }
    }

    if (sock != -1)
    {
        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        shutdown(sock, 0);
        close(sock);
    }

    h_udp_server_task = NULL;
    vTaskDelete(NULL); // calling task will be deleted
}

TaskHandle_t start_udp_broadcast_server_task()
{
    /* UDP server */
    if (xTaskCreatePinnedToCore(udp_broadcast_server_task,
                                "udp_broadcast_server_task",
                                3 * 1024,
                                (void *)NULL,
                                10,
                                &h_udp_server_task,
                                1) != pdPASS)
    {
        ESP_LOGE(TAG, "udp_broadcast_server_task creation failed");
    }

    return h_udp_server_task;
}

// WiFi event handler/callback function
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED)
    {
        ESP_LOGI(TAG, "AP assigned IP to connected Station");
        start_udp_broadcast_server_task();
    }
}

// Initialize the WiFi in Soft AP mode
void wifi_init_softap(wifi_config_t *p_wifi_config)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    w_ap_netif = esp_netif_create_default_wifi_ap();
    assert(w_ap_netif);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, p_wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG,
             "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             p_wifi_config->ap.ssid,
             p_wifi_config->ap.password,
             p_wifi_config->ap.channel);
}

int start_wifi_softap(char *pSsid, char *pPwd)
{
    int ret_val = ESP_FAIL;

    // Length of SSID info field should be between 0 and 32 octets.
    if ((pSsid != NULL) && (strlen(pSsid) > 0))
    {
        ESP_LOGI(TAG, "start_wifi SSID %s", pSsid);

        wifi_config_t wifi_config = {
            .ap = {.ssid = "",
                   .ssid_len = 0,
                   .max_connection = 10,
                   .password = "",
                   .channel = CONFIG_APP_WIFI_CHANNEL_SELECT,
                   .authmode = WIFI_AUTH_WPA_WPA2_PSK},
        };

        strlcpy((char *)wifi_config.ap.ssid, pSsid, sizeof(wifi_config.ap.ssid));
        wifi_config.ap.ssid_len = strlen(pSsid);
        if ((pPwd != NULL) && (strlen(pPwd) > 0))
        {
            strlcpy((char *)wifi_config.ap.password,
                    pPwd,
                    sizeof(wifi_config.ap.password));
        }
        else
        {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        wifi_init_softap(&wifi_config);
        ret_val = ESP_OK;
    }

    return ret_val;
}


void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(start_wifi_softap(CONFIG_HOSTAPP_WIFI_SSID, CONFIG_HOSTAPP_WIFI_PASSWORD));
}
