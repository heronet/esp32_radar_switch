#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_mac.h>

void core0_task()
{

    while (1)
    {
        printf("Running on core: %d\n", xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void core1_task()
{

    while (1)
    {
        printf("Running on core: %d\n", xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    xTaskCreatePinnedToCore(
        core0_task, // Function to implement the task
        "MyTask",   // Name of the task
        2048,       // Stack size in words
        NULL,       // Task input parameter
        1,          // Priority of the task
        NULL,       // Task handle
        0           // Core where the task should run (0 or 1)
    );

    xTaskCreatePinnedToCore(
        core1_task, // Function to implement the task
        "MyTask",   // Name of the task
        2048,       // Stack size in words
        NULL,       // Task input parameter
        1,          // Priority of the task
        NULL,       // Task handle
        1           // Core where the task should run (0 or 1)
    );
}