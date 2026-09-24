/* OMGP host firmware — activation scaffold (RUNBOOK step 7).
 * Exists so the esp32 CI job is truthfully green from commit one.
 * Replaced by the real host task structure when the firmware feature
 * lands: Clock adapter over esp_timer, RS-485 UART transport, control
 * task running the portable host-core superframe scheduler. */
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* l3_smoke.cpp — links the portable L3 codec component into the firmware. */
const char* omgp_l3_smoke(void);
/* link_smoke.cpp — same, for the trunk L2 component (spec 002 T047). These two calls are
 * what pull their object files out of libmain.a: an archive member nothing references is
 * omitted from the link, and with it every omgp_link member it alone would have pulled in.
 * Dropping either call silently un-links that whole component again. */
uint16_t omgp_link_smoke(void);

void app_main(void) {
    printf("OMGP host scaffold — protocol v1.0 pending host-core port (l3: %s, link: 0x%04x)\n",
           omgp_l3_smoke(), omgp_link_smoke());
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
