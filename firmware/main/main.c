// Toolchain smoke test: proves the build, FreeRTOS, and QEMU/hardware boot.
// Replaced by real bring-up in §11 step 1.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"

void app_main(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("WFH board alive: %d cores, rev %d\n", chip.cores, chip.revision);
    printf("free heap: %u, PSRAM: %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    for (int tick = 0;; tick++) {
        printf("tick %d\n", tick);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
