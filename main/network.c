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

/* dest_ip and dest_port are set in fumething_new.c */
extern char *dest_ip;
extern uint32_t dest_port;

static struct sockaddr_in remote_addr;
static int mysocket;
extern xQueueHandle xUdpSendQueue; // from fumething_new.c

/* this just listens for queued messages from app_main() and sends them via udp to somewhere */
void mynet_task(void *pvParameters) {
  int len;
  char udpbuf[128];

  while (1) { 
    while (1) { 
      mysocket = socket(AF_INET, SOCK_DGRAM, 0);
      if (mysocket < 0) {
	ESP_LOGI("net", "socket failure");
	vTaskDelay(1000/portTICK_PERIOD_MS);
	break; // at this point not much to do except wait and retry socket open.
      } else { 
	// dest_ip and dest_port can change via API
	ESP_LOGI("net", "target is %s:%d\n", (char *)&dest_ip, dest_port);
	remote_addr.sin_family = AF_INET;
	remote_addr.sin_addr.s_addr = inet_addr(dest_ip);
	remote_addr.sin_port = htons(dest_port);
	
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
  // if we get here, everything's gone to shit.
}
