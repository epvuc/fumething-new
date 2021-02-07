#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include <esp_log.h>
#include "bme280.h"

extern xQueueHandle xUdpSendQueue;

void my_i2c_setup(void);
void my_bme280_init(void);
void ads1110_writecfg(uint8_t val);
uint8_t ads1110_readcfg(void);
int16_t ads1110_read(void);

extern struct bme280_dev bme280; // from bme280_sup.c
extern bool inet_online;

extern uint32_t  interval; // set in fumething_new.c app_main()
uint32_t  gl_fumes = 0;
float     gl_temp = 0.0;
float     gl_pressure = 0.0;
float     gl_humidity = 0.0;
uint32_t  gl_count = 0;

/* #define DUMMY_MEASUREMENTS */

#ifndef DUMMY_MEASUREMENTS
void measurement_task(void *p)
{
  int8_t rslt;
  int32_t val;
    char msgbuf[128];
  uint32_t count = 0;
  struct bme280_data comp_data;

  // config the actual i2c bus
  my_i2c_setup();
  // assign my hw functions to the bme280 driver & call it's init function
  // & set its sampling configuration
  my_bme280_init();
  
  while(1) { 
    // take BME280 readings. 
    rslt = bme280_set_sensor_mode(BME280_FORCED_MODE, &bme280);
    bme280.delay_ms(40);
    rslt = bme280_get_sensor_data(BME280_ALL, &comp_data, &bme280);
    if (rslt != BME280_OK)
      ESP_LOGI("BME280", "bme280_get_sensor_data() returned %d", rslt);

    // take gas sensor readings.
    ads1110_writecfg(0b10011100); // single conversion, start conv, 15sps, gain 1
    // wait for conversion to finish
    while(ads1110_readcfg() & (1<<7))
      vTaskDelay(1);
    val=ads1110_read();
    //    volts=(double)val/6537.545;

    gl_fumes = val;
    gl_temp = (comp_data.temperature * 9/5)+32;
    gl_pressure = comp_data.pressure / 3386.3886;
    gl_humidity = comp_data.humidity;
    gl_count = count++;

    if (count >= 9999)
      count = 0;

    snprintf(msgbuf, sizeof(msgbuf), "Fumes: f:%d t:%.02f p:%0.4f h:%0.1f #%u\r\n",
	     gl_fumes, gl_temp, gl_pressure, gl_humidity, gl_count);
    ESP_LOGI("FUME", "%s", msgbuf);
    if (inet_online)
      xQueueSend(xUdpSendQueue, &msgbuf, ( TickType_t ) 0);
    vTaskDelay(interval/portTICK_PERIOD_MS);
  }
}

#else /* DUMMY_MEASUREMENTS */ 
void measurement_task(void *p)
{
  char msgbuf[128];
  uint32_t count = 0;
  
  while(1) { 

    gl_count = count++;
    if (count >= 9999)
      count = 0;

    snprintf(msgbuf, sizeof(msgbuf), "Fumes: f:%d t:%.02f p:%0.4f h:%0.1f #%u\r\n",
	     gl_fumes, gl_temp, gl_pressure, gl_humidity, gl_count);

    ESP_LOGI("TEST", "%s", msgbuf);
    if (inet_online)
      xQueueSend(xUdpSendQueue, &msgbuf, ( TickType_t ) 0);
    vTaskDelay(interval/portTICK_PERIOD_MS);
  }
}
#endif
