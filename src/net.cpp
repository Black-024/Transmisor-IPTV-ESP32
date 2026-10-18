#include "net.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "config.h"
#include "events.h"
#include "m3u_index.h"

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static SemaphoreHandle_t s_mtx;
static String s_nowJson;

static bool publish(uint16_t index) {
  Channel c;
  if (!g_m3u.get(index, c)) return false;

  JsonDocument doc;
  doc["type"]  = "play";
  doc["i"]     = index;
  doc["name"]  = c.name;
  doc["group"] = c.group;
  doc["url"]   = c.url;
  String json; serializeJson(doc, json);

  xSemaphoreTake(s_mtx, portMAX_DELAY); s_nowJson = json; xSemaphoreGive(s_mtx);
  ws.textAll(json);
  Serial.printf("[WS] -> %u clientes: %s\n", (unsigned)ws.count(), c.name.c_str());
  return true;
}

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type,
                      void*, uint8_t*, size_t) {
  if (type == WS_EVT_CONNECT) {
    String j;
    xSemaphoreTake(s_mtx, portMAX_DELAY); j = s_nowJson; xSemaphoreGive(s_mtx);
    if (j.length()) client->text(j);
  }
}

void netTask(void*) {
  s_mtx = xSemaphoreCreateMutex();
  xEventGroupWaitBits(g_sysEvents, EVT_WIFI_READY, pdFALSE, pdTRUE, portMAX_DELAY);

  WiFi.setAutoReconnect(true);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/api/now", HTTP_GET, [](AsyncWebServerRequest* r) {
    String j;
    xSemaphoreTake(s_mtx, portMAX_DELAY); j = s_nowJson; xSemaphoreGive(s_mtx);
    r->send(200, "application/json", j.length() ? j : String("{}"));
  });

  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html").setCacheControl("no-cache");
  server.onNotFound([](AsyncWebServerRequest* r) { r->send(404, "text/plain", "404"); });
  server.begin();

  IPAddress lastIp;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != lastIp) {
      lastIp = WiFi.localIP();
      MDNS.end();
      if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
      Serial.printf("[NET] http://%s  (http://%s.local)\n", lastIp.toString().c_str(), HOSTNAME);
    }
    PlayEvent ev;
    if (xQueueReceive(g_playQueue, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) publish(ev.index);
    ws.cleanupClients();
  }
}
