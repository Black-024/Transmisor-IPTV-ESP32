#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

struct PlayEvent { uint16_t index; };

extern QueueHandle_t      g_playQueue;
extern EventGroupHandle_t g_sysEvents;
#define EVT_WIFI_READY BIT0
