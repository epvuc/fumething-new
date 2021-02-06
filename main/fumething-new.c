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
  ESP_ERROR_CHECK( mdns_hostname_set("washingmachine" ));
  mdns_txt_item_t serviceTxtData[2] = {
    {"endpoint","squeak"},
    {"version", version }
  };
  ESP_ERROR_CHECK( mdns_service_add("washer_api", "_http", "_tcp", 80, serviceTxtData, 2) );
}

/* these globals are to let the httpd serve current status */
bool inet_online = pdFALSE;
int8_t relay = 0; // this should be bool but i'm lazy
int32_t avg = 0;
int32_t time_while_filled = 0;

/* these globals control the emulation parameters. 
   air pressure read from MAP sensor sort of corresponds to height of water in washer drum 
   on_pressure must be > off_pressure
   failsafe_filled_level must be < on_pressure or else failsafe is disabled
   failsafe_cycles_max should be around 14 minutes because a fill can take >5m,
   and we can't reliably detect the drop when it starts agitating/spinning until it's drained.  
   so the failsafe should be a whole fill-agitate-drain time.
*/
#define DEF_ON_PRESS 26
#define DEF_OFF_PRESS 8
#define DEF_FAILSAFE_FILLED_LEVEL 15
#define DEF_FAILSAFE_CYCLES_MAX 3300

int32_t on_pressure = DEF_ON_PRESS;  // pressure at which we assume the basin is full and turn on the "full" relay
int32_t off_pressure = DEF_OFF_PRESS;  // pressure at which the basin is drained and we turn off the "full" relay
int32_t failsafe_filled_level = DEF_FAILSAFE_FILLED_LEVEL; // failsafe if above this level for too long, but doesn't reach on_pressure
int32_t failsafe_cycles_max = DEF_FAILSAFE_CYCLES_MAX; // max time to allow partially filled before triggering failsafe (14 min)

/* http server dumps json blob of current params and running state, and has
   endpoints to change params, which get written to nonvolatile storage */
static esp_err_t squeak_get_handler(httpd_req_t *req)
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
      if (httpd_query_key_value(buf, "high", param, sizeof(param)) == ESP_OK) {
	ESP_LOGI("HTTP", "---> high=%s", param);
	if (strtol(param,NULL,10) >= 0)
	  on_pressure=strtol(param, NULL, 10);
	my_nvs_update("on_press", on_pressure);
      }
      if (httpd_query_key_value(buf, "low", param, sizeof(param)) == ESP_OK) {
	ESP_LOGI("HTTP", "---> low=%s", param);
	off_pressure=strtol(param, NULL, 10);
	my_nvs_update("off_press", off_pressure);
      }
      if (httpd_query_key_value(buf, "ffl", param, sizeof(param)) == ESP_OK) {
	ESP_LOGI("HTTP", "---> ffl=%s", param);
	failsafe_filled_level=strtol(param, NULL, 10);
	my_nvs_update("ffl", failsafe_filled_level);
      }
      if (httpd_query_key_value(buf, "fcm", param, sizeof(param)) == ESP_OK) {
	ESP_LOGI("HTTP", "---> fcm=%s", param);
	failsafe_cycles_max=strtol(param, NULL, 10);
	my_nvs_update("fcm", failsafe_cycles_max);
      }
      if (httpd_query_key_value(buf, "defaults", param, sizeof(param)) == ESP_OK) {
	ESP_LOGI("HTTP", "---> defaults=%s", param);
	on_pressure = DEF_ON_PRESS;
	my_nvs_update("on_press", on_pressure);
	off_pressure = DEF_OFF_PRESS;
	my_nvs_update("off_press", off_pressure);
	failsafe_filled_level = DEF_FAILSAFE_FILLED_LEVEL; 
	my_nvs_update("ffl", failsafe_filled_level);
	failsafe_cycles_max = DEF_FAILSAFE_CYCLES_MAX;
	my_nvs_update("fcm", failsafe_cycles_max);
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
  
  snprintf(resp_buf, 255, "{\"ver\": \"%s\", \"on_press\": %d, \"off_press\": %d, \"cur\": %d, \"twf\": %d, \"ffl\": %d, \"fcm\": %d, \"rly\": %d}\n", version, on_pressure, off_pressure, avg, time_while_filled, failsafe_filled_level, failsafe_cycles_max, relay);
  httpd_resp_send(req, resp_buf, strlen(resp_buf));
  if (will_restart) {
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
  }
  return ESP_OK;
}

static const httpd_uri_t squeak = {
  .uri       = "/squeak",
  .method    = HTTP_GET,
  .handler   = squeak_get_handler,
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
    httpd_register_uri_handler(server, &squeak);
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
        tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, "washingmachine");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
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
    const char *ssid_prefix = "HEEPY_";
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

#define DEFAULT_VREF    1100        //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   64          //Multisampling

#define OLIMEX_REL1_PIN 32

#if CONFIG_IDF_TARGET_ESP32
static esp_adc_cal_characteristics_t *adc_chars;
static const adc_channel_t channel = ADC_CHANNEL_3;     //GPIO34 if ADC1, GPIO14 if ADC2
static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
#elif CONFIG_IDF_TARGET_ESP32S2
static const adc_channel_t channel = ADC_CHANNEL_6;     // GPIO7 if ADC1, GPIO17 if ADC2
static const adc_bits_width_t width = ADC_WIDTH_BIT_13;
#endif
static const adc_atten_t atten = ADC_ATTEN_DB_0;
static const adc_unit_t unit = ADC_UNIT_1;

#if CONFIG_IDF_TARGET_ESP32
static void check_efuse(void)
{
    //Check TP is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) {
        printf("eFuse Two Point: Supported\n");
    } else {
        printf("eFuse Two Point: NOT supported\n");
    }

    //Check Vref is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK) {
        printf("eFuse Vref: Supported\n");
    } else {
        printf("eFuse Vref: NOT supported\n");
    }
}

static void print_char_val_type(esp_adc_cal_value_t val_type)
{
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        printf("Characterized using Two Point Value\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        printf("Characterized using eFuse Vref\n");
    } else {
        printf("Characterized using Default Vref\n");
    }
}
#endif

void app_main(void)
{
  /* initialize nvs flash partition if it's messed up */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  
  // read or initialize the operational params in nvs
  // (nvs_key_name, default_value, variable)
  my_nvs_read_or_initialize("on_press", DEF_ON_PRESS, &on_pressure);
  my_nvs_read_or_initialize("off_press", DEF_OFF_PRESS, &off_pressure);
  my_nvs_read_or_initialize("ffl", DEF_FAILSAFE_FILLED_LEVEL, &failsafe_filled_level);
  my_nvs_read_or_initialize("fcm", DEF_FAILSAFE_CYCLES_MAX, &failsafe_cycles_max);
    
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
    const char *pop = "abcd1234";
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
    wifi_prov_mgr_deinit();

    /* Start Wi-Fi station */
    wifi_init_sta();
  }

  // this is the udp data sender task -
  xUdpSendQueue = xQueueCreate(10, 128);
  xTaskCreate(mynet_task, "my_net_task", 3072, NULL, 1, NULL);
  
  char msgbuf[256];
#if CONFIG_IDF_TARGET_ESP32
  //Check if Two Point or Vref are burned into eFuse
  check_efuse();
#endif
  int32_t values[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
  //  int32_t avg;
  int32_t tot;
  uint8_t pos = 0;
  
    gpio_pad_select_gpio(OLIMEX_REL1_PIN);
    gpio_set_direction(OLIMEX_REL1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(OLIMEX_REL1_PIN, 0);
    relay = 0;

    //Configure ADC
    if (unit == ADC_UNIT_1) {
        adc1_config_width(width);
        adc1_config_channel_atten(channel, atten);
    } else {
        adc2_config_channel_atten((adc2_channel_t)channel, atten);
    }

#if CONFIG_IDF_TARGET_ESP32
    //Characterize ADC
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);
#endif
    uint32_t first_voltage = 0;
    uint8_t first_read = 1;

    if (inet_online) { 
      sprintf(msgbuf, "Washer startup.\n");
      xQueueSend(xUdpSendQueue, &msgbuf, ( TickType_t ) 0);
    }

    //Continuously sample ADC1
    while (1) {
        uint32_t adc_reading = 0;
        //Multisampling
        for (int i = 0; i < NO_OF_SAMPLES; i++) {
            if (unit == ADC_UNIT_1) {
                adc_reading += adc1_get_raw((adc1_channel_t)channel);
            } else {
                int raw;
                adc2_get_raw((adc2_channel_t)channel, width, &raw);
                adc_reading += raw;
            }
        }
        adc_reading /= NO_OF_SAMPLES;

        //Convert adc_reading to voltage in mV
        uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
	if (first_read == 1) {
	  first_voltage = voltage;
	  first_read = 0;
	}
	if (voltage < first_voltage) voltage = first_voltage; // don't let it go negative
	int32_t relative = voltage - first_voltage;


	// top water level is abt 960 mv

	// keep a moving average
	tot = 0;
	for (uint8_t i = 0; i< 16; i++) 
	  tot = tot +  values[i];
	avg = tot/16;
	values[pos] = relative;
	pos++;
	if (pos == 16) pos = 0;


	// sometimes when filling it never reaches the on_pressure threshold.
	// this is a failsave so it won't just get stuck like that forever. 
	if ( avg > failsafe_filled_level )
	  time_while_filled++;
	else
	  time_while_filled = 0;
	// if fill level is above failsafe_filled_level for a long time but never
	// reaches on_pressure, turn on the relay anyway to let the cycle proceed.
	if (time_while_filled > failsafe_cycles_max) {
	  gpio_set_level(OLIMEX_REL1_PIN, 1);
	  relay = 1;
	  snprintf(msgbuf, 127, "filling for too long, tripped failsafe\n");
	  ESP_LOGI("failsafe", "%s", msgbuf);
	  if (inet_online)
	    xQueueSend(xUdpSendQueue, &msgbuf, ( TickType_t ) 0);
	}
	
	// if adc voltage from MAP sensor exceeds high threshold, engage the relay
	if (avg > on_pressure) {
	  gpio_set_level(OLIMEX_REL1_PIN, 1);
	  relay=1;
        } else {
	  // if adc voltage from MAP sensor falls below low threshold, disengage the relay
	  if (avg < off_pressure) {
	    gpio_set_level(OLIMEX_REL1_PIN, 0);
	    relay=0;
	  }
	}
    
	snprintf(msgbuf,255,"{\"v\": %d, \"rel\": %d, \"avg\": %d, \"twf\": %d, \"rly\": %d}\n",
		 voltage, relative, avg, time_while_filled, relay);
	printf("%s", msgbuf);
	if (inet_online)
	  xQueueSend(xUdpSendQueue, &msgbuf, ( TickType_t ) 0);
	else
	  ESP_LOGI("NET", "we're offline, can't send data.");
        vTaskDelay(pdMS_TO_TICKS(250));
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
