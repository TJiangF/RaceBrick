#include "discovery.h"
#include "cloud_sync.h"
#include "netcfg.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "discover";
#define DISCOVERY_PORT 4210
#define DISCOVERY_REQ "RACEBRICK_DISCOVER"

static void discovery_task(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(s);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "udp discovery on %d", DISCOVERY_PORT);
    for (;;) {
        char req[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, req, sizeof(req) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        req[n] = 0;
        if (!strstr(req, DISCOVERY_REQ)) continue;
        char out[160];
        int len = snprintf(out, sizeof(out), "RACEBRICK|%s|%s|phone-relay-1\n",
                           cloud_sync_uid(), netcfg_ip());
        sendto(s, out, len, 0, (struct sockaddr *)&from, fl);
    }
}

void discovery_start(void) {
    if (xTaskCreate(discovery_task, "discover", 4096, NULL, 3, NULL) != pdPASS)
        ESP_LOGE(TAG, "task create failed");
}
