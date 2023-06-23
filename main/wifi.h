#ifndef WIFI_H_
#define WIFI_H_

#include "main.h"

#define STA_SSID_NVS_KEY "ssid"
#define STA_PWD_NVS_KEY "pass"


#define DEFAULT_AP_SSID "ESP-CONNECT-MNGR"
#define DEFAULT_AP_PASSWD "illia2408"

#define MAX_RECONNECT_TRIES 20

#define DEFAULT_SCAN_LIST_SIZE 20

void networkTask(void* params);

#endif