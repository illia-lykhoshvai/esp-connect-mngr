#include "stdio.h"
#include "string.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_mac.h"

#define COMPONENT_TAG "wifi"
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE // to display all levels
#include "esp_log.h"

#include "cJSON.h"

#include "main.h"
#include "wifi.h"
#include "webserver.h"

static uint8_t wifiRetryNum = 0;

#define WIFI_FOUND_STA_SSID         BIT0
#define WIFI_FOUND_STA_PWD          BIT1
#define WIFI_STA_CONNECTED_BIT      BIT2
#define WIFI_STA_FAIL_BIT           BIT3
#define WIFI_STA_TRY_BIT            BIT4
static EventGroupHandle_t wifiEventGroup;

static nvs_handle_t wifiSettingsHandle;

void changeWiFiSTA(char* appSSID, char* appPasswd) {
    esp_wifi_disconnect();

    ESP_LOGI(COMPONENT_TAG, "Changing Wi-Fi mode to STA %s", NON_NULL_STR(appSSID));
    wifi_config_t sta_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    memcpy(sta_config.sta.ssid, appSSID, strlen(NON_NULL_STR(appSSID)));
    memcpy(sta_config.sta.password, appPasswd, strlen(NON_NULL_STR(appPasswd)));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
    esp_wifi_connect();
}

void changeWiFiAPSTA(void) {
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_netif_create_default_wifi_ap();
    
    ESP_LOGI(COMPONENT_TAG, "Changing Wi-Fi mode to AP-STA");
    wifi_config_t ap_config = { 0 };
	strcpy((char *)ap_config.ap.password, DEFAULT_AP_PASSWD);
	strcpy((char *)ap_config.ap.ssid,DEFAULT_AP_SSID);
	ap_config.ap.ssid_len = strlen(DEFAULT_AP_SSID);
	ap_config.ap.authmode = (strlen(DEFAULT_AP_PASSWD) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
	ap_config.ap.max_connection = 8;
	ap_config.ap.channel = 6;

    wifi_config_t sta_config = { 0 };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
    esp_wifi_start();
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    // ip_event_got_ip_t* event = NULL;
    ESP_LOGI(COMPONENT_TAG, "stepped into wifi event handler because of: %s .. %d", event_base, (int)event_id);
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
        {
            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED:
        {
            if (wifiRetryNum < MAX_RECONNECT_TRIES) {
                esp_wifi_connect();
                wifiRetryNum++;
                ESP_LOGI(COMPONENT_TAG, "Retry to connect to the AP ... %d", wifiRetryNum);
            } else {
                xEventGroupSetBits(wifiEventGroup, WIFI_STA_FAIL_BIT);
                if (xEventGroupGetBits(wifiEventGroup) & WIFI_STA_TRY_BIT) {
                    xEventGroupClearBits(wifiEventGroup, WIFI_STA_TRY_BIT);
                }
                changeWiFiAPSTA();
            }
            ESP_LOGI(COMPONENT_TAG,"Connect to the AP fail");
            break;
        }
        case WIFI_EVENT_STA_CONNECTED:
        {
            if (xEventGroupGetBits(wifiEventGroup) & WIFI_STA_TRY_BIT) {
                wifi_config_t* currentConfig = NULL;
                currentConfig = pvPortMalloc(sizeof(wifi_config_t));
                bzero(currentConfig, sizeof(wifi_config_t));
                if (currentConfig == NULL) {
                    ESP_LOGE(COMPONENT_TAG, "Failed to alloc memory to read Wi-Fi conf.");
                    break;
                }
                esp_wifi_get_config( WIFI_IF_STA, currentConfig);

                nvs_set_str(wifiSettingsHandle, STA_SSID_NVS_KEY, (char*) currentConfig->sta.ssid);
                ESP_LOGI(COMPONENT_TAG, "Writing to NVS SSID: %s", (char*) currentConfig->sta.ssid);
                nvs_set_str(wifiSettingsHandle, STA_PWD_NVS_KEY, (char*) currentConfig->sta.password);
                ESP_LOGV(COMPONENT_TAG, "Writing to NVS password: %s", (char*) currentConfig->sta.password);

                free(currentConfig);
                xEventGroupClearBits(wifiEventGroup, WIFI_STA_TRY_BIT);
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: 
        {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(COMPONENT_TAG, "station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: 
        {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(COMPONENT_TAG, "station "MACSTR" leave, AID=%d", MAC2STR(event->mac), event->aid);
            break;
        }
        }
    } else if (event_base == IP_EVENT) {
        switch(event_id) {
        case IP_EVENT_STA_GOT_IP: 
        {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(COMPONENT_TAG, "Connected! Got ip:" IPSTR, IP2STR(&event->ip_info.ip));
            wifiRetryNum = 0;
            xEventGroupSetBits(wifiEventGroup, WIFI_STA_CONNECTED_BIT);
            break;
        }
        }
    }
}

void initWiFiSTA(char* appSSID, char* appPasswd) {
    ESP_LOGI(COMPONENT_TAG, "Initiating wifi STA ... %s", NON_NULL_STR(appSSID));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
            &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
            &event_handler, NULL, &instance_got_ip));

    wifi_config_t sta_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    memcpy(sta_config.sta.ssid, appSSID, strlen(NON_NULL_STR(appSSID)));
    memcpy(sta_config.sta.password, appPasswd, strlen(NON_NULL_STR(appPasswd)));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // no power saving
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(COMPONENT_TAG, "WIFI STA init finished.");
}

void initWiFiAPSTA(void) {
    ESP_LOGV(COMPONENT_TAG, "Initiating wifi AP...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
	ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );

    wifi_config_t ap_config = { 0 };
	strcpy((char *)ap_config.ap.password, DEFAULT_AP_PASSWD);
	strcpy((char *)ap_config.ap.ssid,DEFAULT_AP_SSID);
	ap_config.ap.ssid_len = strlen(DEFAULT_AP_SSID);
	ap_config.ap.authmode = (strlen(DEFAULT_AP_PASSWD) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
	ap_config.ap.max_connection = 8;
	ap_config.ap.channel = 6;

    wifi_config_t sta_config = { 0 };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(COMPONENT_TAG, "WIFI AP-STA init finished.");
}

esp_err_t readStringFromNVS(nvs_handle_t* handle, char** destination, char* key);

void networkTask(void* params) {
    httpd_handle_t webServer = NULL;
    char *appSSID = NULL, *appPasswd = NULL;
    esp_err_t err;
    ESP_LOGI(COMPONENT_TAG, "Started wifiTask");
    wifiEventGroup = xEventGroupCreate();

    // try to init nvs in flash
    err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) 
        || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_LOGV(COMPONENT_TAG, "Failed to init nvs from first try");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_LOGV(COMPONENT_TAG, "Opening NVS handle...");
    err = nvs_open("wifi-settings", NVS_READWRITE, &wifiSettingsHandle);
    if (err != ESP_OK) {
        ESP_LOGE(COMPONENT_TAG, "Failed to open NVS. Error: (%d)", err);
        vTaskDelete(NULL);
    } else {
        ESP_LOGI(COMPONENT_TAG, "Got NVS handle. Reading WI-FI settings...");

        err = readStringFromNVS(&wifiSettingsHandle, &appSSID, STA_SSID_NVS_KEY);
        if (err == ESP_OK) {
            ESP_LOGV(COMPONENT_TAG, "foundSSID: %s", appSSID);
            xEventGroupSetBits(wifiEventGroup, WIFI_FOUND_STA_SSID);
        }

        err = readStringFromNVS(&wifiSettingsHandle, &appPasswd, STA_PWD_NVS_KEY);
        if (err == ESP_OK) {
            ESP_LOGV(COMPONENT_TAG, "foundPWD: %s", appPasswd);
            xEventGroupSetBits(wifiEventGroup, WIFI_FOUND_STA_PWD);
        }
    }

    if ((xEventGroupGetBits(wifiEventGroup) & (WIFI_FOUND_STA_SSID | WIFI_FOUND_STA_PWD)) == (WIFI_FOUND_STA_SSID | WIFI_FOUND_STA_PWD)) {
        ESP_LOGI(COMPONENT_TAG, "starting ssid: %s, pwd: %s", appSSID, appPasswd);
        initWiFiSTA(appSSID, appPasswd);
    } else {
        initWiFiAPSTA();
    }

    esp_err_t ret = mountStorage("/storage");
    if (ret != ESP_OK) {
        ESP_LOGE(COMPONENT_TAG, "got error: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
    }

    wsReceiveQ = xQueueCreate(10, sizeof(char*));
    wsTransmitQ = xQueueCreate(10, sizeof(wsMsg_t));

    webServer = start_webserver();

    char* receiveString = NULL;
    wsMsg_t* transmitObj = NULL;
    while(1) {
        if (xQueueReceive(wsReceiveQ, &receiveString, pdMS_TO_TICKS(20)) == pdTRUE) {
            ESP_LOGI(COMPONENT_TAG, "Got from Q: %s", receiveString);

            enum wsResponses responseType = emptyResponse;
            cJSON *requestJSON = cJSON_Parse((char *) receiveString);
            cJSON *requestType = NULL;

            if (requestJSON != NULL) {
                requestType = cJSON_GetObjectItemCaseSensitive(requestJSON, "request");
                if (cJSON_IsString(requestType) && (requestType->valuestring != NULL)) {
                    if (strcmp(requestType->valuestring, decodeWsRequest(scanResponse)) == 0) {
                        responseType = scanResponse;
                    } else if (strcmp(requestType->valuestring, decodeWsRequest(connectResponse)) == 0) {
                        responseType = connectResponse;
                    }
                }
            }

            transmitObj = pvPortMalloc(sizeof(wsMsg_t));
            if (transmitObj == NULL) {
                ESP_LOGE(COMPONENT_TAG, "Doesn't have enough memory to generate WS response");
                break;
            }
            transmitObj->jsonString = NULL;

            cJSON *obj = cJSON_CreateObject();
            switch (responseType) {
                case scanResponse:
                    cJSON *scanArray, *scanObject;
                    uint16_t number = DEFAULT_SCAN_LIST_SIZE;
                    wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];

                    scanArray = cJSON_AddArrayToObject(obj, "scanResults");
                        cJSON_AddStringToObject(obj, "responseType", "scanResponse");

                    memset(ap_info, 0, sizeof(ap_info));
                    esp_wifi_scan_start(NULL, true);
                    if (esp_wifi_scan_get_ap_records(&number, ap_info) == ESP_OK) {
                        ESP_LOGI(COMPONENT_TAG, "Total APs scanned = %u", number);
                        for (int i = 0; (i < DEFAULT_SCAN_LIST_SIZE) && (i < number); i++) {
                            scanObject = cJSON_CreateObject();
                            cJSON_AddStringToObject(scanObject, "ssid" , (char*)ap_info[i].ssid);
                            cJSON_AddNumberToObject(scanObject, "rssi" , ap_info[i].rssi);
                            cJSON_AddItemToArray(scanArray, scanObject);
                        }
                        cJSON_AddStringToObject(obj, "status", "ok");
                    } else {
                        cJSON_AddStringToObject(obj, "status", "fail");
                    }
                    break;
                case connectResponse:
                    cJSON *ssid = NULL, *passwd = NULL;
                    ssid = cJSON_GetObjectItemCaseSensitive(requestJSON, "ssid");
                    passwd = cJSON_GetObjectItemCaseSensitive(requestJSON, "pass");

                    xEventGroupSetBits( wifiEventGroup, WIFI_STA_TRY_BIT);
                    changeWiFiSTA(ssid->valuestring, passwd->valuestring);
                    cJSON_AddStringToObject(obj, "status", "ok");
                    break;
                default:
            }

            transmitObj->jsonString = cJSON_Print(obj);
            cJSON_Delete(obj);
            if (transmitObj->jsonString != NULL) {
                transmitObj->len = strlen(transmitObj->jsonString);
            }
            
            ESP_LOGV(COMPONENT_TAG, "Sending to Q[%d]: %s", transmitObj->len, transmitObj->jsonString);
            xQueueSend(wsTransmitQ, transmitObj, pdMS_TO_TICKS(200));
            
            cJSON_Delete(requestJSON);
            vPortFree(receiveString);
            vPortFree(transmitObj);
        }
    }

    stop_webserver(webServer);
    vTaskDelete(NULL);
}

esp_err_t readStringFromNVS(nvs_handle_t* nvsHandle, char** destination, char* key) {
    size_t nvsRequiredSize;
    esp_err_t err;
    ESP_LOGV(COMPONENT_TAG, "Trying NVS key: [%s]!", key);
    err = nvs_get_str(*nvsHandle, key, NULL, &nvsRequiredSize);
    switch (err) {
        case ESP_OK:
            ESP_LOGI(COMPONENT_TAG, "Successful find of [%s]!", key);
            *destination = pvPortMalloc(nvsRequiredSize);
            bzero(*destination, nvsRequiredSize);
            err = nvs_get_str(*nvsHandle, key, *destination, &nvsRequiredSize );
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            ESP_LOGE(COMPONENT_TAG, "Specified key not found!");
            break;
        default :
            ESP_LOGE(COMPONENT_TAG, "Error (%s) reading key:[%s]!", esp_err_to_name(err), key);
    }
    return err;
}
