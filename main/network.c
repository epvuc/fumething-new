#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/netdb.h"

#define DEST_IP "10.0.0.1"
#define DEST_PORT 31553

static struct sockaddr_in remote_addr;
static int mysocket;
extern xQueueHandle xUdpSendQueue;

/* this just listens for queued messages from app_main() and sends them via udp to somewhere */
void mynet_task(void *pvParameters) {
  int len;
  char udpbuf[128];

  while (1) { 
    mysocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (mysocket < 0) {
      ESP_LOGI("net", "socket failure");
    } else { 
      remote_addr.sin_family = AF_INET;
      remote_addr.sin_port = htons(DEST_PORT);
      remote_addr.sin_addr.s_addr = inet_addr(DEST_IP);

      while (1) {
	if (xQueueReceive(xUdpSendQueue, &udpbuf, (TickType_t)2)) {
	  vTaskDelay(1); 
	  len = sendto(mysocket, udpbuf, strlen(udpbuf), 0, (struct sockaddr *)&remote_addr, sizeof(remote_addr));
	  if (len > 0) {
	    // happytime
	  } else {
	    ESP_LOGI("net", "udp sendto failed");
	    close(mysocket);
	    break;
	  }
	}
      }
    }    
  }
}
