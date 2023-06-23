#ifndef WEBSERVER_H_
#define WEBSERVER_H_

struct asyncRespArg {
    httpd_handle_t hd;
    int fd;
};

typedef struct wsReceiveMsgStruct {
    char* jsonString;
    uint16_t len;
} wsMsg_t;

enum wsResponses {
    emptyResponse = 0,
    scanResponse,
    connectResponse,
    lastResponse
};

char* decodeWsRequest(uint8_t id);

extern QueueHandle_t wsReceiveQ, wsTransmitQ;

void stop_webserver(httpd_handle_t server);

httpd_handle_t start_webserver(void);

esp_err_t mountStorage(char* base_path);

esp_err_t sendSpiffsFile(httpd_req_t* req, char* fName);

#endif
