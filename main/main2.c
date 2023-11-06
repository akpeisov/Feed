#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"

#define RMT_TX_CHANNEL 1
#define RMT_TX_GPIO_NUM 13

#define PIN_EN 2
#define PIN_RESET 14
#define PIN_DIR 15

#define STEPS_PER_REVOLUTION 200
#define MICROSTEPS 16

#define SPEED 1000

void stepMotor(uint16_t steps, bool direction);

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    return ESP_OK;
}

void app_main()
{
    gpio_pad_select_gpio(PIN_EN);
    gpio_pad_select_gpio(PIN_DIR);
    gpio_pad_select_gpio(PIN_RESET);
    gpio_set_direction(PIN_RESET, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_EN, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_DIR, GPIO_MODE_OUTPUT);

    rmt_config_t rmt_tx;
    rmt_tx.rmt_mode = RMT_MODE_TX;
    rmt_tx.channel = RMT_TX_CHANNEL;
    rmt_tx.gpio_num = RMT_TX_GPIO_NUM;
    rmt_tx.mem_block_num = 3;
    rmt_tx.clk_div = 80;

    rmt_config(&rmt_tx);
    rmt_driver_install(rmt_tx.channel, 0, 0);

    printf("Starting stepper motor control...\n");

    //wifi_init_sta();

    gpio_set_level(PIN_RESET, 0);
    gpio_set_level(PIN_RESET, 1);
    gpio_set_level(PIN_EN, 1);

    // while (1)
    {
        // Rotate clockwise for 360 degrees
        
        // stepMotor(200, true);        
        // vTaskDelay(2000 / portTICK_PERIOD_MS);

        // stepMotor(100, false);        
        // vTaskDelay(2000 / portTICK_PERIOD_MS);

        for (int i=0; i<100; i++) {
            stepMotor(19, true);        
            stepMotor(12, false);     
        }   
        vTaskDelay(2000 / portTICK_PERIOD_MS);

        // Rotate counter-clockwise for 360 degrees
        //stepMotor(50*100, false);
        //vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

void stepMotor(uint16_t steps, bool direction)
{
    gpio_set_level(PIN_EN, 0);
    rmt_item32_t items[2];
    items[0].level0 = 1;
    items[0].duration0 = SPEED;
    items[0].level1 = 0;
    items[0].duration1 = SPEED;

    // items[1].level0 = 1;
    // items[1].duration0 = 1000;
    // items[1].level1 = 0;
    // items[1].duration1 = 1000;

    gpio_set_level(PIN_DIR, direction ? 1 : 0);

    for (uint16_t i=0;i<steps;i++)
        rmt_write_items(RMT_TX_CHANNEL, items, 1, true);
    //rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);
    gpio_set_level(PIN_EN, 1);
/*
    int count = abs(steps);    
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t delay = 2 / portTICK_PERIOD_MS;

    for (int i = 0; i < count; i++)
    {
        rmt_write_items(RMT_TX_CHANNEL, items, 2, true);

        vTaskDelayUntil(&lastWakeTime, delay);
    }
    */
}