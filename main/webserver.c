#include "stdio.h"
#include "string.h"

#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"

#include "cJSON.h"

#define COMPONENT_TAG "http-server"
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE // to display all levels
#include "esp_log.h"

#include "main.h"
#include "webserver.h"

static httpd_handle_t server = NULL;

QueueHandle_t wsReceiveQ, wsTransmitQ;

char* wsRequestsStrings[] = {
    "empty",
    "scan",
    "connect"
};

/* Our URI handler function to be called during GET /uri request */
esp_err_t get_main_handler(httpd_req_t *req) {
    /* Send a simple response */
    esp_err_t ret = sendSpiffsFile(req, "/storage/main.html");
    if (ret == ESP_FAIL) {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

void ws_async_send(void *arg) {
    wsMsg_t buffWsMessage = {0};
    httpd_ws_frame_t ws_pkt;
    struct asyncRespArg* resp_arg = arg;
    httpd_handle_t hd = resp_arg->hd;

    if (xQueueReceive(wsTransmitQ, &buffWsMessage, pdMS_TO_TICKS(4000)) == pdTRUE) {
        ESP_LOGV(COMPONENT_TAG, "Sending string[%d]: %s", buffWsMessage.len, buffWsMessage.jsonString);
    } else {
        ESP_LOGE(COMPONENT_TAG, "Failed to get item from wsTransmitQ");
        buffWsMessage.jsonString = "{\"status\":\"fail\"}";
        buffWsMessage.len = strlen(buffWsMessage.jsonString);
    }
    
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*) buffWsMessage.jsonString;
    ws_pkt.len = buffWsMessage.len;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    static size_t max_clients = CONFIG_LWIP_MAX_LISTENING_TCP;
    size_t fds = max_clients;
    int client_fds[max_clients];

    esp_err_t ret = httpd_get_client_list(server, &fds, client_fds);

    if (ret != ESP_OK) {
        return;
    }

    for (int i = 0; i < fds; i++) {
        int client_info = httpd_ws_get_fd_info(server, client_fds[i]);
        if (client_info == HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_ws_send_frame_async(hd, client_fds[i], &ws_pkt);
        }
    }
    vPortFree(resp_arg);
}

esp_err_t trigger_async_send(httpd_handle_t handle, httpd_req_t *req) {
    struct asyncRespArg *resp_arg = malloc(sizeof(struct asyncRespArg));
    resp_arg->hd = req->handle;
    resp_arg->fd = httpd_req_to_sockfd(req);
    return httpd_queue_work(handle, ws_async_send, resp_arg);
}

esp_err_t handle_ws_req(httpd_req_t *req) {
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    char *qMessage = NULL;
    esp_err_t ret;

    if (req->method == HTTP_GET) {
        ESP_LOGI(COMPONENT_TAG, "Handshake done, the new ws connection was opened");
        return ESP_OK;
    }

    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(COMPONENT_TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }

    if (ws_pkt.len) {
        buf = pvPortMalloc(ws_pkt.len + 1);
        bzero(buf, (ws_pkt.len + 1));
        if (buf == NULL) {
            ESP_LOGE(COMPONENT_TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(COMPONENT_TAG, "httpd_ws_recv_frame failed with %d", ret);
            vPortFree(buf);
            return ret;
        }
        ESP_LOGV(COMPONENT_TAG, "Got packet with message: %s", ws_pkt.payload);
    }

    ESP_LOGV(COMPONENT_TAG, "frame len is %d", ws_pkt.len);

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        qMessage = pvPortMalloc(ws_pkt.len+1);
        bzero(qMessage, (ws_pkt.len+1));
        if (qMessage == NULL) {
            ESP_LOGE(COMPONENT_TAG, "Failed to alloc memory to send to Q");
            return ESP_FAIL;
        }
        memcpy(qMessage, ws_pkt.payload, ws_pkt.len);
        xQueueSend(wsReceiveQ, &qMessage, pdMS_TO_TICKS(1000));
        vPortFree(buf);
        return trigger_async_send(req->handle, req);
    }
    return ESP_OK;
}

/* Function for starting the webserver */
httpd_handle_t start_webserver(void) {
    /* Generate default configuration */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        /* URI handler structure for GET / */
    httpd_uri_t main_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_main_handler,
        .user_ctx = NULL
    };
    /* WebSocket handler structure */
    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws_req,
        .user_ctx = NULL,
        .is_websocket = true
    };

    /* Start the httpd server */
    if (httpd_start(&server, &config) == ESP_OK) {
        /* Register URI handlers */
        httpd_register_uri_handler(server, &main_get);
        httpd_register_uri_handler(server, &ws);
    }
    /* If server failed to start, handle will be NULL */
    return server;
}

/* Function for stopping the webserver */
void stop_webserver(httpd_handle_t server) {
    if (server) {
        /* Stop the httpd server */
        httpd_stop(server);
    }
}

esp_err_t mountStorage(char* base_path) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = base_path,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(COMPONENT_TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(COMPONENT_TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(COMPONENT_TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(COMPONENT_TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(COMPONENT_TAG, "Partition size: total: %d, used: %d", total, used);
    return ESP_OK;
}

#define CHUNK_SIZE 512
esp_err_t sendSpiffsFile(httpd_req_t* req, char* fName) {
    char bufferToSend[CHUNK_SIZE];
    esp_err_t returnValue = ESP_OK;
    size_t length = 0, readCount = 0;

    FILE *file = fopen(fName, "r");
    if (file == NULL) { // if didn't open
        ESP_LOGE(COMPONENT_TAG, "Failed to open file %s", fName);
        returnValue = ESP_FAIL;
        goto fileTransmissionEnd;
    }

    fseek(file, 0, SEEK_END); // search till eof
    length = ftell(file); // tell the length of file by index of eof
    fseek(file, 0, SEEK_SET); // return to the first char in file

    do {
        memset(bufferToSend, 0, CHUNK_SIZE);
        readCount = fread(bufferToSend, sizeof(bufferToSend[0]), CHUNK_SIZE, file);
        if (readCount != CHUNK_SIZE) { // didn't read whole CHUNK size
            if (ferror(file)) { // check if file error, otherwise its eof
                ESP_LOGE(COMPONENT_TAG, "Failed due to read file fail!");
                returnValue = ESP_FAIL;
                goto fileTransmissionEnd;
            }
        }
        ESP_LOGI(COMPONENT_TAG, "Sending str[%d] out of %d", readCount, length);
        httpd_resp_send_chunk(req, bufferToSend, readCount);
        length -= readCount;
    } while (length > 0);

    fileTransmissionEnd:
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(file);

    return returnValue;
}

char* decodeWsRequest(uint8_t id) {
    return wsRequestsStrings[id];
}
