/*
 * Sensor de ultrassom HC-SR04 com FreeRTOS
 */
#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <stdio.h>
#include <task.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include "pins.h"
#include "ssd1306.h"

// === Recursos do RTOS ===
static QueueHandle_t xQueueTime;
static QueueHandle_t xQueueDistance;
static SemaphoreHandle_t xSemaphoreTrigger;

// === Função de callback (IRQ) ===
// Envia o instante de cada borda do echo (subida e descida)
void pin_callback(uint gpio, uint32_t events) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint64_t tempo = to_us_since_boot(get_absolute_time());

    xQueueSendFromISR(xQueueTime, &tempo, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void oled_display_init(ssd1306_t *disp) {
    i2c_init(i2c1, 400000);
    gpio_set_function(2, GPIO_FUNC_I2C);
    gpio_set_function(3, GPIO_FUNC_I2C);
    gpio_pull_up(2);
    gpio_pull_up(3);

    disp->external_vcc = false;
    ssd1306_init(disp, 128, 64, 0x3C, i2c1);
    ssd1306_clear(disp);
    ssd1306_show(disp);

    gpio_init(SSD1306_PIN_LITE);
    gpio_set_dir(SSD1306_PIN_LITE, GPIO_OUT);
    gpio_put(SSD1306_PIN_LITE, 0);
}

void led_rgb_init(void) {
    gpio_init(LED_PIN_R);
    gpio_set_dir(LED_PIN_R, GPIO_OUT);
    gpio_put(LED_PIN_R, 1);

    gpio_init(LED_PIN_G);
    gpio_set_dir(LED_PIN_G, GPIO_OUT);
    gpio_put(LED_PIN_G, 1);

    gpio_init(LED_PIN_B);
    gpio_set_dir(LED_PIN_B, GPIO_OUT);
    gpio_put(LED_PIN_B, 1);
}

void trigger_task(void *p) {
    gpio_init(TRIGGER_PIN);
    gpio_set_dir(TRIGGER_PIN, GPIO_OUT);

    while (1) {
        // Pulso de 10us no trigger
        gpio_put(TRIGGER_PIN, 1);
        sleep_us(10);
        gpio_put(TRIGGER_PIN, 0);

        // Avisa o OLED que uma leitura foi disparada
        xSemaphoreGive(xSemaphoreTrigger);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void echo_task(void *p) {
    gpio_init(ECHO_PIN);
    gpio_set_dir(ECHO_PIN, GPIO_IN);
    gpio_set_irq_enabled_with_callback(ECHO_PIN,
                                       GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
                                       true, &pin_callback);

    uint64_t inicio;
    uint64_t fim;

    while (1) {
        xQueueReceive(xQueueTime, &inicio, portMAX_DELAY);

        if (xQueueReceive(xQueueTime, &fim, pdMS_TO_TICKS(50))) {
            // distância em cm = tempo em us / 58
            int distancia = (int)((fim - inicio) / 58);
            xQueueSend(xQueueDistance, &distancia, 0);
        }
    }
}

void oled_task(void *p) {
    ssd1306_t disp;
    oled_display_init(&disp);
    led_rgb_init();

    int distancia;
    char texto[20];

    while (1) {
        xSemaphoreTake(xSemaphoreTrigger, portMAX_DELAY);
        ssd1306_clear(&disp);

        if (xQueueReceive(xQueueDistance, &distancia, pdMS_TO_TICKS(100))) {
            sprintf(texto, "Dist: %d cm", distancia);
            ssd1306_draw_string(&disp, 0, 0, 1, texto);

            // Barra: 400 cm = 128 pixels
            ssd1306_draw_square(&disp, 0, 20, distancia * 128 / 400, 6);

            // LED ativo em 0: verde até 1m, amarelo (vermelho + verde) acima
            gpio_put(LED_PIN_G, 0);
            gpio_put(LED_PIN_R, distancia <= 100);
        } else {
            ssd1306_draw_string(&disp, 0, 0, 1, "Sensor falhou!");

            // LED vermelho
            gpio_put(LED_PIN_R, 0);
            gpio_put(LED_PIN_G, 1);
        }

        ssd1306_show(&disp);
    }
}

int main() {
    stdio_init_all();

    xQueueTime = xQueueCreate(32, sizeof(uint64_t));
    xQueueDistance = xQueueCreate(32, sizeof(int));
    xSemaphoreTrigger = xSemaphoreCreateBinary();

    xTaskCreate(trigger_task, "Trigger", 4095, NULL, 1, NULL);
    xTaskCreate(echo_task, "Echo", 4095, NULL, 1, NULL);
    xTaskCreate(oled_task, "OLED", 4095, NULL, 1, NULL);

    vTaskStartScheduler();

    while (true);
}