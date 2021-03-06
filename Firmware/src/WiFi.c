/**
 * @file       WiFi.c
 * @brief      WiFi driver
 * @ingroup    Firmware
 * @author     Michael Fitzmayer
 * @copyright  "THE BEER-WARE LICENCE" (Revision 42)
 */

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_smartconfig.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "nvs_flash.h"
#include "rom/ets_sys.h"
#include "tcpip_adapter.h"
#include "WiFi.h"

/**
 * @struct  WiFiDriver
 * @brief   WiFi driver data
 */
typedef struct WiFiDriver_t
{
    bool bHasIP;
    /* FreeRTOS event group to signal when we are connected & ready
       to make a request */
    EventGroupHandle_t hEventGroup;

} WiFiDriver;

/**
 * @var    _stDriver
 * @brief  WiFi driver private data
 */
static WiFiDriver _stDriver;

static esp_err_t _EventHandler(void* pctx, system_event_t* stEvent);
static void      _SmartConfigThread(void* pArg);
static void      _SCCallback(smartconfig_status_t stStatus, void* pdata);

/**
 * @enum     eBits_t
 * @brief    
 * @details  The event group allows multiple bits for each event, but we
 *           only care about one event - are we connected to the AP with
 *           an IP?
 */
typedef enum eBits_t
{
    eCONNECTED_BIT     = BIT0,
    eESPTOUCH_DONE_BIT = BIT1

} eBits;

/**
 * @fn     void InitWiFi(void)
 * @brief  Initialise WiFi driver
 */
void InitWiFi(void)
{
    wifi_init_config_t stConfig = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(nvs_flash_init());
    memset(&_stDriver, 0, sizeof(struct WiFiDriver_t));

    tcpip_adapter_init();
    _stDriver.hEventGroup = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_loop_init(_EventHandler, NULL));

    ESP_ERROR_CHECK(esp_wifi_init(&stConfig));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/**
 * @fn     void WaitForIP(void)
 * @brief  Wait for IP (blocking)
 */
void WaitForIP(void)
{
    while (! _stDriver.bHasIP)
    {
        ets_delay_us(100000);
    }
}

static esp_err_t _EventHandler(void* pctx, system_event_t* stEvent)
{
    switch (stEvent->event_id)
    {
        case SYSTEM_EVENT_STA_START:
            xTaskCreate(_SmartConfigThread, "SmartConfigThread", 4096, NULL, 3, NULL);
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(_stDriver.hEventGroup, eCONNECTED_BIT);
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(_stDriver.hEventGroup, eCONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void _SmartConfigThread(void* pArg)
{
    EventBits_t uxBits;
    (void)pArg;

    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
    ESP_ERROR_CHECK(esp_smartconfig_start(_SCCallback));

    while (1)
    {
        uxBits = xEventGroupWaitBits(
            _stDriver.hEventGroup, eCONNECTED_BIT | eESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);

        if (uxBits & eCONNECTED_BIT)
        {
            ESP_LOGI("sc", "WiFi Connected to ap");
        }
        if (uxBits & eESPTOUCH_DONE_BIT)
        {
            _stDriver.bHasIP = true;
            ESP_LOGI("sc", "SmartConfig over");
            esp_smartconfig_stop();
            vTaskDelete(NULL);
        }
    }
}

static void _SCCallback(smartconfig_status_t stStatus, void* pdata)
{
    wifi_config_t* stConfig = pdata;

    switch (stStatus)
    {
        case SC_STATUS_WAIT:
            ESP_LOGI("sc", "SC_STATUS_WAIT");
            break;
        case SC_STATUS_FIND_CHANNEL:
            ESP_LOGI("sc", "SC_STATUS_FINDING_CHANNEL");
            break;
        case SC_STATUS_GETTING_SSID_PSWD:
            ESP_LOGI("sc", "SC_STATUS_GETTING_SSID_PSWD");
            break;
        case SC_STATUS_LINK:
            ESP_LOGI("sc", "SC_STATUS_LINK");
            ESP_LOGI("sc", "SSID:%s", stConfig->sta.ssid);
            ESP_LOGI("sc", "PASSWORD:%s", stConfig->sta.password);
            ESP_ERROR_CHECK(esp_wifi_disconnect());
            ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, stConfig));
            ESP_ERROR_CHECK(esp_wifi_connect());
            break;
        case SC_STATUS_LINK_OVER:
            ESP_LOGI("sc", "SC_STATUS_LINK_OVER");
            if (pdata != NULL)
            {
                uint8_t au8PhoneIP[4] = { 0 };
                memcpy(au8PhoneIP, (uint8_t*)pdata, 4);
                ESP_LOGI(
                    "sc",
                    "Phone IP: %d.%d.%d.%d\n",
                    au8PhoneIP[0],
                    au8PhoneIP[1],
                    au8PhoneIP[2],
                    au8PhoneIP[3]);
            }
            xEventGroupSetBits(_stDriver.hEventGroup, eESPTOUCH_DONE_BIT);
            break;
        default:
            break;
    }
}
