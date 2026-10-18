#include <Arduino.h>
#include <LittleFS.h>
#include "config.h"
#include "events.h"
#include "m3u_index.h"
#include "ui.h"
#include "net.h"

QueueHandle_t      g_playQueue;
EventGroupHandle_t g_sysEvents;

void setup() {
  Serial.begin(115200);
  g_playQueue = xQueueCreate(1, sizeof(PlayEvent));
  g_sysEvents = xEventGroupCreate();

  uiInit();
  if (!LittleFS.begin(true)) uiFatal("LittleFS fallo");
  uiSplash("Leyendo lista M3U...");
  if (!g_m3u.build(PLAYLIST_PATH)) uiFatal("Falta /playlist.m3u");
  Serial.printf("Canales: %u | grupos: %u | heap libre: %u\n",
                (unsigned)g_m3u.count(), (unsigned)g_m3u.groupCount(), ESP.getFreeHeap());

  xTaskCreatePinnedToCore(uiTask,  "ui",  10240, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(netTask, "net", 6144, nullptr, 1, nullptr, 0);
}

void loop() { vTaskDelay(portMAX_DELAY); }
