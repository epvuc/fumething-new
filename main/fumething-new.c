#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <driver/gpio.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_ota_ops.h> // this is just to be able to pull the project version string
#include <nvs_flash.h>
#include <nvs.h>

#include <esp_system.h>
#include <esp_netif.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <lwip/sockets.h>
#include <lwip/dns.h>
#include <lwip/netdb.h>

// these are to allow initial mobile app setup of wifi
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include <esp_http_server.h>
#include <mdns.h>

#define BLE_PROV_PROOF_OF_POSSESSION "abcd1234"
#define DEF_DEST_IP "10.0.0.1\0"
#define DEF_DEST_PORT 31553
#define DEF_INTERVAL 1000

char dest_ip[16] = DEF_DEST_IP;
uint32_t dest_port = DEF_DEST_PORT;
uint32_t interval = DEF_INTERVAL;

extern uint32_t  gl_fumes;
extern float     gl_temp;
extern float     gl_pressure;
extern float     gl_humidity;
extern uint32_t  gl_count;


// GPIO ports for on-board LED and button
#define LED1 2
#define BOOT_BUTTON 0

void led_on(void) {  gpio_set_level(LED1, 1); }
void led_off(void) {  gpio_set_level(LED1, 0); }

void my_nvs_read_or_initialize(char*, int32_t, int32_t*);
void my_nvs_update(char*, int32_t);

static void initialise_mdns(void)
{
  char *version = "unknown_version";
  // get the PROJECT_VERSION string the cmake system sets from git
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_app_desc_t running_app_info;
  if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
    ESP_LOGI("SYS", "Running firmware version: %s", running_app_info.version);
    version = running_app_info.version;
  }

  ESP_ERROR_CHECK( mdns_init() );
  ESP_ERROR_CHECK( mdns_hostname_set("fumething" ));
  mdns_txt_item_t serviceTxtData[2] = {
    {"endpoint","fume"},
    {"version", version }
  };
  ESP_ERROR_CHECK( mdns_service_add("fumething_api", "_http", "_tcp", 80, serviceTxtData, 2) );
}

/* these globals are to let the httpd serve current status */
bool inet_online = pdFALSE;

/* http server dumps json blob of current params and running state, and has
   endpoints to change params, which get written to nonvolatile storage */
static esp_err_t api_get_handler(httpd_req_t *req)
{
  char*  buf;
  char resp_buf[256];
  size_t buf_len;
  bool will_restart = pdFALSE;

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = malloc(buf_len);
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      ESP_LOGI("HTTP", "Found URL query => %s", buf);
      char param[32];
      /* put more api endpoints here */
      if (httpd_query_key_value(buf, "dest_port", param, sizeof(param)) == ESP_OK) {
	ESP_LOGI("HTTP", "---> dest_port=%s", param);
	dest_port=strtol(param, NULL, 10);
	// my_nvs_update("dest_port", dest_port);
      }
      if (httpd_query_key_value(buf, "interval", param, sizeof(param)) == ESP_OK) {
	ESP_LOGI("HTTP", "---> interval=%s", param);
	interval=strtol(param, NULL, 10);
	// my_nvs_update("interval", interval);
      }
      if (httpd_query_key_value(buf, "reset", param, sizeof(param)) == ESP_OK) {
	ESP_LOGI("HTTP", "---> reset=%s", param);
	if(strncmp(param, "1", 1) == 0)
	  will_restart = pdTRUE;
      }
    }
    free(buf);
  }
  char *version = "unknown_version";
  // get the PROJECT_VERSION string the cmake system sets from git
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_app_desc_t running_app_info;
  if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK)
    version = running_app_info.version;
  
  // this string reaches a max size of 182 bytes, if you add anything make sure it still fits in resp_buf!
  snprintf(resp_buf, 255,
	   "{\"ver\": \"%s\", \"dest_ip\": \"%s\", \"dest_port\": \"%d\", \"interval\": \"%d\", \"count\": \"%d\", \"fumes\": \"%d\", \"temp\": \"%.1f\", \"press\": \"%.4f\", \"rh\": \"%.1f\"}\n",
	   version, dest_ip, dest_port, interval, gl_count, gl_fumes, gl_temp, gl_pressure, gl_humidity );
  httpd_resp_send(req, resp_buf, strlen(resp_buf));
  if (will_restart) {
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
  }
  return ESP_OK;
}

static const httpd_uri_t fume = {
  .uri       = "/fume",
  .method    = HTTP_GET,
  .handler   = api_get_handler,
};

static httpd_handle_t start_webserver(void)
{
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  // Start the httpd server
  ESP_LOGI("HTTP", "Starting server on port: '%d'", config.server_port);
  if (httpd_start(&server, &config) == ESP_OK) {
    // Set URI handlers
    ESP_LOGI("HTTP", "Registering URI handlers");
    httpd_register_uri_handler(server, &fume);
    return server;
  }
  ESP_LOGI("HTTP", "Error starting server!");
  return NULL;
}

static void stop_webserver(httpd_handle_t server)
{
  // Stop the httpd server
  httpd_stop(server);
}

const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;

/* Event handler for catching system events */
static void event_handler(void* arg, esp_event_base_t event_base,
                          int event_id, void* event_data)
{
    httpd_handle_t webserver = NULL;
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI("WIFI", "Provisioning started");
		led_on();
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI("WIFI", "Received Wi-Fi credentials"
                         "\n\tSSID     : %s\n\tPassword : %s",
                         (const char *) wifi_sta_cfg->ssid,
                         (const char *) wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE("WIFI", "Provisioning failed!\n\tReason : %s"
                         "\n\tPlease reset to factory and retry provisioning",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                         "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI("WIFI", "Provisioning successful");
                break;
            case WIFI_PROV_END:
                /* De-initialize manager once provisioning is finished */
                wifi_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, "fumething");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
	led_off();
        ESP_LOGI("WIFI", "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        /* Signal main application to continue execution */
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
	inet_online = pdTRUE;
	webserver = start_webserver();
	initialise_mdns();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI("WIFI", "Disconnected. Connecting to the AP again...");
	inet_online = pdFALSE;
	stop_webserver(webserver);
	mdns_free();
	webserver = NULL;
        esp_wifi_connect();
    }
}

static void wifi_init_sta(void)
{
    /* Start Wi-Fi in station mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X",
             ssid_prefix, eth_mac[4], eth_mac[5]);
}


/* these are mine, for transmitting udp packets from a task */
// queue handles have to be declared as xQueueHandle now, not QueueHandle_t!
// If you do it the old way, it panics the cpu if bluetooth is enabled!
// I wasted two hours of my life discovering this!

xQueueHandle xUdpSendQueue = NULL;
void mynet_task(void *);
extern void measurement_task(void *p);

void app_main(void)
{
  gpio_pad_select_gpio(LED1); 
  gpio_set_direction(LED1, GPIO_MODE_OUTPUT);
  gpio_set_level(LED1, 0);

  gpio_pad_select_gpio(BOOT_BUTTON);
  gpio_set_direction(BOOT_BUTTON, GPIO_MODE_INPUT);
  
  /* initialize nvs flash partition if it's messed up */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  // read or initialize the operational params in nvs
  // (nvs_key_name, default_value, variable)
  /*  -- TODO: make this work with strings too, for IP addr -- 
  my_nvs_read_or_initialize("dest_ip", DEF_DEST_IP, &dest_ip);
  my_nvs_read_or_initialize("dest_port", DEF_DEST_PORT, &dest_port);
  my_nvs_read_or_initialize("interval", DEF_INTERVAL, &interval);
  */
  
  /* provision wifi and IP */
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_event_group = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  wifi_prov_mgr_config_t config = {
      .scheme = wifi_prov_scheme_ble,
      .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
  };
  ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
  bool provisioned = false;
  ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));
  if (!provisioned) {
    ESP_LOGI("PROV", "Starting provisioning");
    char service_name[12];
    get_device_service_name(service_name, sizeof(service_name));
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    // const char *pop = "abcd1234";
    const char *pop = BLE_PROV_PROOF_OF_POSSESSION;
    const char *service_key = NULL;
    uint8_t custom_service_uuid[] = {
      0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
      0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
    };
    wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));
  } else {
    ESP_LOGI("PROV", "Already provisioned, starting Wi-Fi STA");

    /* We don't need the manager as device is already provisioned, so let's release it's resources */
    // wifi_prov_mgr_deinit();  // disabled so we can reprovision if we want, maybe this is ok?

    /* Start Wi-Fi station */
    wifi_init_sta();
  }

  // this is the udp data sender task -
  xUdpSendQueue = xQueueCreate(10, 128);
  xTaskCreate(mynet_task, "my_net_task", 3072, NULL, 1, NULL);
  xTaskCreate(measurement_task, "measurement_task", 3072, NULL, 1, NULL);  

  char msgbuf[256];
  
  if (inet_online) { 
    sprintf(msgbuf, "fumething startup.\n");
    xQueueSend(xUdpSendQueue, &msgbuf, ( TickType_t ) 0);
  }

  while (1) {
    if (gpio_get_level(BOOT_BUTTON) == 0) { 
      ESP_LOGI("GPIO", "gpio0: button pushed\n");
      inet_online = pdFALSE;
      mdns_free(); 
      esp_wifi_disconnect();
      ESP_LOGI("PROV", "Starting provisioning");
      char service_name[12];
      get_device_service_name(service_name, sizeof(service_name));
      wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
      // const char *pop = "abcd1234";
      const char *pop = BLE_PROV_PROOF_OF_POSSESSION;
      const char *service_key = NULL;
      uint8_t custom_service_uuid[] = {
	0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
	0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
      };
      wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);
      ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    /*
    snprintf(msgbuf, 255, "0000000000 Fumes: f:0000 t:00.00 p:00.0000 h:00.0 #0000\n");
    printf("%s", msgbuf);
    if (inet_online)
      xQueueSend(xUdpSendQueue, &msgbuf, ( TickType_t ) 0);
    else
      ESP_LOGI("NET", "we're offline, can't send data.");
    vTaskDelay(pdMS_TO_TICKS(interval));
    */
  }
}

void my_nvs_update(char *key, int32_t value) {
  esp_err_t err;
  nvs_handle_t my_handle;

  err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err != ESP_OK) 
    ESP_LOGI("NVS", "error (%s) opening handle", esp_err_to_name(err));
  else {
    err = nvs_set_i32(my_handle, key, value);
    ESP_LOGI("NVS", "write storage %s = %d: %s", key, value, (err != ESP_OK) ? "FAILED" : "OK");
    err = nvs_commit(my_handle);
    ESP_LOGI("NVS", "%s", (err != ESP_OK) ? "commit FAILED" : "commit succeeded");
  }
}

void my_nvs_read_or_initialize(char *key, int32_t defval, int32_t *parameter) {
  esp_err_t err;
  *parameter = defval;
  nvs_handle_t my_handle;
  err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err != ESP_OK) 
    ESP_LOGI("NVS", "error (%s) opening NVS handle", esp_err_to_name(err));
  else {
    *parameter = defval;
    err = nvs_get_i32(my_handle, key, parameter);
    switch (err) {
    case ESP_OK:
      ESP_LOGI("NVS", "nvs read %s = %d", key, *parameter);
      break;
    case ESP_ERR_NVS_NOT_FOUND:
      ESP_LOGI("NVS", "nvs key %s not initialized, updating to %d.", key, *parameter);
      my_nvs_update(key, *parameter);
      break;
    default :
      ESP_LOGI("NVS", "error (%s) reading nvs", esp_err_to_name(err));
    }
  }
}
