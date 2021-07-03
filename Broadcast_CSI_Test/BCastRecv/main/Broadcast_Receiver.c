#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "BCAST_R";
static wifi_config_t wifi_config;
char g_sta_ip_addr_str[IP4ADDR_STRLEN_MAX] = {0};
char bcast_ip_addr_str[IP4ADDR_STRLEN_MAX] = {0};

size_t g_tot_num_csidata_cb = 0; // total CSI calls
size_t g_udp_sendto_count = 0;
size_t g_udp_sendto_error_count = 0;
size_t g_udp_recv_count = 0;
size_t g_udp_recv_error_count = 0;

static int udp_sock = -1;

#define APP_UDP_SENDTO_ERR_DELAY (50)
#define CONFIG_APP_UDP_SERVER_PORT (3333)
#define CONFIG_APP_WIFI_CHANNEL_SELECT (11)
#define CONFIG_HOSTAPP_WIFI_SSID "CSI_BCAST"
#define CONFIG_HOSTAPP_WIFI_PASSWORD "CSI_BCAST_PASSWORD123"

// WiFi CSI data callback function
static void IRAM_ATTR wifi_csi_raw_cb(void *ctx, wifi_csi_info_t *info)
{
    ++g_tot_num_csidata_cb;
    ESP_LOGI(TAG, "Total CSI callbacks: %zu", g_tot_num_csidata_cb);
}

void csi_init()
{
    g_tot_num_csidata_cb = 0;
    ESP_ERROR_CHECK(esp_wifi_set_csi(false));

    wifi_csi_config_t configuration_csi = {0};
    configuration_csi.lltf_en = true;
    configuration_csi.htltf_en = true;
    configuration_csi.stbc_htltf2_en = true;
    configuration_csi.ltf_merge_en = true;
    configuration_csi.channel_filter_en = false;
    configuration_csi.manu_scale = false;

    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(&wifi_csi_raw_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&configuration_csi));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));

    ESP_LOGI(TAG, "csi init successful");
}

// Update socket to receive the broadcasted message
bool socket_receive_bcast(int sock, struct sockaddr_in *p_bcast_addr, const char *p_bcast_ipv4_addr)
{
    int err = -1;

    if ((sock == -1) || (p_bcast_addr == NULL) || (p_bcast_ipv4_addr == NULL))
    {
        ESP_LOGE(TAG, "Invalid socket");
        return false;
    }
    int reuseaddr = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) < 0)
    {
        ESP_LOGE(TAG, "Error in setting reuseaddr option, errno %d - %s", errno, strerror(errno));
        closesocket(sock);
        return false;
    }

    bzero(p_bcast_addr, sizeof(struct sockaddr_in));
    p_bcast_addr->sin_addr.s_addr = inet_addr(p_bcast_ipv4_addr); // htonl(INADDR_BROADCAST);
    p_bcast_addr->sin_family = AF_INET;
    p_bcast_addr->sin_port = htons(CONFIG_APP_UDP_SERVER_PORT);

    err = bind(sock, (struct sockaddr *)p_bcast_addr, sizeof(struct sockaddr_in));
    if (err < 0)
    {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d - %s", errno, strerror(errno));
        shutdown(sock, 0);
        closesocket(sock);
        return false;
    }
    ESP_LOGI(TAG, "Socket bound to port %d", CONFIG_APP_UDP_SERVER_PORT);
    return true;
}

static esp_timer_handle_t h_periodic_timer[0];

void periodic_timer_callback(void *arg)
{
    ESP_LOGI(TAG, "#Recv: %zu, #CSI cb: %u per sec. Free heap %d B", g_udp_recv_count, g_tot_num_csidata_cb, esp_get_free_heap_size());
    g_udp_recv_count = 0; g_tot_num_csidata_cb = 0;
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

void udp_broadcast_receive_task(void *pParams)
{
    // Start the timer to count num of UDP packet received per second
    setup_periodic_timer(&periodic_timer_callback, (1 * 1000000));

    // Target address could be broadcast address when CONFIG_APP_BROADCAST_MODE is enabled
    const char *target_t_bcast_addr = bcast_ip_addr_str;

    ESP_LOGI(TAG, "udp_broadcast_receive_task started. Target address %s", target_t_bcast_addr);

    char rx_buffer[128];
    char addr_str[128];
    g_udp_sendto_count = 0;
    g_udp_sendto_error_count = 0;
    g_udp_recv_count = 0;
    g_udp_recv_error_count = 0;

    while (1)
    {
        struct sockaddr_in dest_r_bcast_addr;
        dest_r_bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST); // inet_addr(target_t_bcast_addr);
        dest_r_bcast_addr.sin_family = AF_INET;
        dest_r_bcast_addr.sin_port = htons(CONFIG_APP_UDP_SERVER_PORT);
        inet_ntoa_r(dest_r_bcast_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

        udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (udp_sock < 0)
        {
            ESP_LOGE(TAG, "Unable to create socket: errno %d - %s", errno, strerror(errno));
            break;
        }
        ESP_LOGI(TAG, "Socket %d created %s:%d", udp_sock, target_t_bcast_addr, CONFIG_APP_UDP_SERVER_PORT);

        if (!socket_receive_bcast(udp_sock, &dest_r_bcast_addr, target_t_bcast_addr))
        {
            break;
        }

        int len = 0;
        socklen_t addr_len = 0;
        addr_len = sizeof(struct sockaddr_in);

        while (1)
        {
            len = recvfrom(udp_sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&dest_r_bcast_addr, &addr_len);
            if (len < 0)
            {
                ++g_udp_recv_error_count;
                // ESP_LOGE(TAG, "recvfrom failed: errno %d - %s", errno, strerror(errno));
            }
            else
            {
                rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
                // ESP_LOGI(TAG, "Received %d bytes. Msg: %s", len, rx_buffer);
                ++g_udp_recv_count;
            }
        } // end of while loop

        if (udp_sock != -1)
        {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(udp_sock, 0);
            close(udp_sock);
            udp_sock = -1;
        }
    }
}

// WiFi event handler/callback function
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {

        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_ip4_addr_t s_gw_ip_addr = event->ip_info.gw;
        esp_ip4addr_ntoa(&s_gw_ip_addr, &g_sta_ip_addr_str[0], IP4ADDR_STRLEN_MAX);

        char *token = strtok(g_sta_ip_addr_str, ".");

        uint16_t inc = 0, ip_class = 1;
        // loop through the string to extract all other tokens
        while (token != NULL)
        {
            inc += sprintf(&bcast_ip_addr_str[inc], "%s.", token);
            if (ip_class >= 3)
            {
                inc += sprintf(&bcast_ip_addr_str[inc], "255");
                break;
            }
            ++ip_class;
            token = strtok(NULL, ".");
        }
        ESP_LOGI(TAG, "Framed broadcast address: %s", bcast_ip_addr_str);

        /* udp_broadcast_receive_task */
        if (xTaskCreatePinnedToCore(udp_broadcast_receive_task,
                                    "udp_broadcast_receive_task",
                                    3 * 1024,
                                    &bcast_ip_addr_str[0], // func param
                                    5,
                                    NULL,
                                    1) != pdPASS)
        {
            ESP_LOGE(TAG, "udp_broadcast_receive_task creation failed");
        }
    }
}

// Initialize the WiFi in Station mode
void wifi_init_sta(wifi_config_t *p_wifi_config)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.csi_enable = 1;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, p_wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start()); // Start the WiFi driver

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* CSI */
    csi_init();
}

int start_wifi_station(char *pSsid, char *pPwd)
{
    int ret_val = ESP_OK;

    // Length of SSID info field should be between 0 and 32 octets.
    if ((pSsid != NULL) && (strlen(pSsid) > 0))
    {
        ESP_LOGI(TAG, "start_wifi_station SSID %s", pSsid);

        memset(&wifi_config, 0, sizeof(wifi_config_t));
        wifi_config.sta.channel = CONFIG_APP_WIFI_CHANNEL_SELECT;
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;

        // specific channel search strategy isn't needed
        strlcpy(
            (char *)wifi_config.sta.ssid, pSsid, sizeof(wifi_config.sta.ssid));
        if ((pPwd != NULL) && (strlen(pPwd) > 0))
        {
            strlcpy((char *)wifi_config.sta.password,
                    pPwd,
                    sizeof(wifi_config.sta.password));
        }

        wifi_init_sta(&wifi_config);
    }

    return ret_val;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    start_wifi_station(CONFIG_HOSTAPP_WIFI_SSID, CONFIG_HOSTAPP_WIFI_PASSWORD);
}
