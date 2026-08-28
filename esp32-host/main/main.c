/* OMGP host firmware — activation scaffold (RUNBOOK step 7).
 * Exists so the esp32 CI job is truthfully green from commit one.
 * Replaced by the real host task structure when the firmware feature
 * lands: Clock adapter over esp_timer, RS-485 UART transport, control
 * task running the portable host-core superframe scheduler. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* l3_smoke.cpp — links the portable L3 codec component into the firmware. */
const char* omgp_l3_smoke(void);

void app_main(void) {
    printf("OMGP host scaffold — protocol v1.0 pending host-core port (l3: %s)\n",
           omgp_l3_smoke());
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
