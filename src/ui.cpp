#include "ui.h"
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ESP32Encoder.h>
#include <functional>
#include <vector>
#include "config.h"
#include "events.h"
#include "m3u_index.h"
#include "wifi_secrets.h"
#include "wifi_store.h"

#define LOG(...) Serial.printf(__VA_ARGS__)

static TFT_eSPI tft;
#if ENCODER_ENABLED
static ESP32Encoder enc;
#endif
static String s_now;

enum class Key { None, Up, Down, Ok, Back };
using LabelFn = std::function<String(size_t)>;
using OkFn    = std::function<void(size_t)>;

static const int HEADER_H = 40, FOOTER_H = 28, ROW_H = 36, BACK_W = 118, ARROW_W = 64;

static bool     s_wifiPhase   = false;
static bool     s_serialCreds = false;
static String   s_serSsid, s_serPass, s_serBuf;
static uint32_t s_rxBytes     = 0;

static const char* statusText(wl_status_t s);

static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    s_rxBytes++;
    if (c == '\r') continue;
    if (c != '\n') { if (s_serBuf.length() < 160) s_serBuf += c; continue; }
    String line = s_serBuf; s_serBuf = ""; line.trim();
    if (line.length() == 0) continue;

    String head = line.substring(0, 5); head.toLowerCase();
    if (head == "wifi ") {
      String body = line.substring(5);
      int bar = body.indexOf('|');
      s_serSsid = (bar >= 0) ? body.substring(0, bar) : body;
      s_serPass = (bar >= 0) ? body.substring(bar + 1) : String("");
      s_serialCreds = s_serSsid.length() > 0;
      LOG("[SERIAL] Comando wifi recibido: red '%s', clave de %u caracteres\n",
          s_serSsid.c_str(), (unsigned)s_serPass.length());
    } else if (line.equalsIgnoreCase("status")) {
      LOG("[SERIAL] Wi-Fi: %s  IP=%s\n", statusText(WiFi.status()), WiFi.localIP().toString().c_str());
    } else {
      LOG("[SERIAL] Recibi '%.12s' pero no es un comando. Uso:  wifi NOMBRE_RED|CLAVE   (o: status)\n", line.c_str());
    }
  }
}

static const char* keyName(Key k) {
  switch (k) { case Key::Up: return "ENC arriba"; case Key::Down: return "ENC abajo";
               case Key::Ok: return "boton OK"; case Key::Back: return "boton ATRAS"; default: return "-"; }
}

static Key readKeyRaw() {
#if ENCODER_ENABLED
  static int64_t last = 0;
  static uint32_t tOk = 0, tBack = 0;
  static bool pOk = true, pBack = true;

  int64_t c = enc.getCount();
  if (c - last >= ENC_STEPS) { last += ENC_STEPS; return Key::Down; }
  if (last - c >= ENC_STEPS) { last -= ENC_STEPS; return Key::Up; }

  uint32_t now = millis(); Key k = Key::None;
  bool okNow = digitalRead(PIN_ENC_SW), backNow = digitalRead(PIN_BACK);
  if (!okNow   && pOk   && now - tOk   > DEBOUNCE_MS) { tOk = now;   k = Key::Ok; }
  if (!backNow && pBack && now - tBack > DEBOUNCE_MS) { tBack = now; k = Key::Back; }
  pOk = okNow; pBack = backNow;
  return k;
#else
  return Key::None;
#endif
}

static Key readKey() {
  if (s_wifiPhase) {
    pollSerial();
    if (s_serialCreds) return Key::Back;
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat > 5000) {
      lastBeat = millis();
      LOG("[WIFI] Esperando configuracion: toca la pantalla o Serial 'wifi RED|CLAVE'. Bytes recibidos por Serial: %u\n",
          (unsigned)s_rxBytes);
    }
  }
  Key k = readKeyRaw();
  if (k != Key::None) LOG("[IN] %s\n", keyName(k));
  return k;
}
__attribute__((unused)) static Key waitKey() { Key k; while ((k = readKey()) == Key::None) vTaskDelay(pdMS_TO_TICKS(5)); return k; }

enum class TEv { None, Tap, Drag, Release };
struct Gesture { TEv ev = TEv::None; int x = 0, y = 0, dy = 0; };

static Gesture pollGesture() {
  Gesture g;
#ifdef TOUCH_CS
  static bool down = false, moved = false;
  static int sx = 0, sy = 0, ly = 0;
  static uint32_t lastSeen = 0;
  uint16_t tx = 0, ty = 0;
  uint32_t now = millis();
  if (tft.getTouch(&tx, &ty, TOUCH_Z_MIN)) {
    lastSeen = now;
    if (!down) { down = true; moved = false; sx = tx; sy = ty; ly = ty; }
    else {
      if (abs((int)ty - sy) > DRAG_PX) moved = true;
      if (moved) { g.ev = TEv::Drag; g.dy = (int)ty - ly; }
      ly = ty;
    }
  } else if (down && now - lastSeen > 80) {
    down = false;
    if (!moved) { g.ev = TEv::Tap; g.x = sx; g.y = sy; LOG("[TOUCH] toque en x=%d y=%d\n", sx, sy); }
    else g.ev = TEv::Release;
  }
#endif
  return g;
}

static void message(const String& l1, const String& l2 = "", uint16_t col = TFT_WHITE) {
  tft.fillScreen(TFT_BLACK); tft.setTextColor(col, TFT_BLACK); tft.setTextDatum(MC_DATUM);
  tft.drawString(l1, tft.width() / 2, tft.height() / 2 - 12, 2);
  if (l2.length()) tft.drawString(l2, tft.width() / 2, tft.height() / 2 + 12, 2);
  tft.setTextDatum(TL_DATUM);
}
void uiSplash(const char* m) { message(m); }
void uiFatal(const char* m)  { LOG("[FATAL] %s\n", m); message("ERROR", m, TFT_RED); for (;;) delay(1000); }

static int visibleRows() { return (tft.height() - HEADER_H - FOOTER_H) / ROW_H; }

static void drawList(const String& title, size_t n, const LabelFn& label, int sel, int top, const char* backLabel) {
  const int W = tft.width(), H = tft.height(), rows = visibleRows(), listW = W - ARROW_W;
  tft.setTextPadding(0);

  tft.fillRect(0, 0, W, HEADER_H, TFT_NAVY);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY); tft.drawString(title, 8, HEADER_H / 2, 4);
  tft.fillRoundRect(W - BACK_W - 4, 4, BACK_W, HEADER_H - 8, 6, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY); tft.drawString(backLabel, W - BACK_W / 2 - 4, HEADER_H / 2, 2);

  tft.setTextDatum(ML_DATUM);
  for (int r = 0; r < rows; r++) {
    int i = top + r, y = HEADER_H + r * ROW_H;
    uint16_t bg = (i == sel) ? TFT_DARKCYAN : TFT_BLACK;
    tft.fillRect(0, y, listW, ROW_H, bg);
    if ((size_t)i < n) { tft.setTextColor(TFT_WHITE, bg); tft.drawString(label(i), 8, y + ROW_H / 2, 4); }
    tft.drawFastHLine(0, y + ROW_H - 1, listW, TFT_DARKGREY);
  }

  const int areaH = rows * ROW_H, cx = listW + ARROW_W / 2;
  const int cyUp = HEADER_H + areaH / 4, cyDn = HEADER_H + (areaH * 3) / 4;
  tft.fillRect(listW, HEADER_H, ARROW_W, areaH, 0x2104);
  uint16_t cUp = (top > 0) ? TFT_WHITE : TFT_DARKGREY;
  uint16_t cDn = (top + rows < (int)n) ? TFT_WHITE : TFT_DARKGREY;
  tft.fillTriangle(cx, cyUp - 14, cx - 18, cyUp + 10, cx + 18, cyUp + 10, cUp);
  tft.fillTriangle(cx, cyDn + 14, cx - 18, cyDn - 10, cx + 18, cyDn - 10, cDn);

  tft.fillRect(0, H - FOOTER_H, W, FOOTER_H, TFT_DARKGREY);
  tft.setTextColor(TFT_YELLOW, TFT_DARKGREY); tft.drawString(s_now, 8, H - FOOTER_H / 2, 2);
  int last = top + rows; if (last > (int)n) last = (int)n;
  String pos = String(top + 1) + "-" + String(last) + " / " + String((unsigned)n);
  tft.setTextDatum(MR_DATUM); tft.drawString(pos, W - 8, H - FOOTER_H / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

static int listMenu(const String& title, size_t n, const LabelFn& label, int sel = 0,
                    const OkFn& onOk = nullptr, const char* backLabel = "< Atras") {
  if (n == 0) { message("Lista vacia"); vTaskDelay(pdMS_TO_TICKS(2000)); return -1; }
  LOG("[UI] Menu '%s' (%u elementos)\n", title.c_str(), (unsigned)n);
  const int rows = visibleRows(), W = tft.width();
  const int maxTop = ((int)n > rows) ? (int)n - rows : 0;
  if (sel < 0) sel = 0;
  if (sel >= (int)n) sel = (int)n - 1;
  int top = (sel < rows) ? 0 : constrain(sel, 0, maxTop);
  int dragAcc = 0;
  bool redraw = true;
  tft.fillScreen(TFT_BLACK);

  auto keepSelVisible = [&]() {
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
  };
  auto clampSelToView = [&]() {
    int hi = top + rows - 1; if (hi > (int)n - 1) hi = (int)n - 1;
    sel = constrain(sel, top, hi);
  };

  for (;;) {
    if (redraw) { drawList(title, n, label, sel, top, backLabel); redraw = false; }

    Key k = readKey();
    if (k != Key::None) {
      if (k == Key::Down)      { sel = (sel + 1) % n; keepSelVisible(); redraw = true; }
      else if (k == Key::Up)   { sel = (sel + n - 1) % n; keepSelVisible(); redraw = true; }
      else if (k == Key::Back) return -1;
      else if (k == Key::Ok)   { if (!onOk) return sel; onOk(sel); redraw = true; }
      continue;
    }

    Gesture g = pollGesture();
    if (g.ev == TEv::None) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

    if (g.ev == TEv::Release) { dragAcc = 0; continue; }

    if (g.ev == TEv::Drag) {
      dragAcc += g.dy;
      int step = 0;
      while (dragAcc <= -ROW_H) { step++; dragAcc += ROW_H; }
      while (dragAcc >=  ROW_H) { step--; dragAcc -= ROW_H; }
      if (step) { top = constrain(top + step, 0, maxTop); clampSelToView(); redraw = true; }
      continue;
    }

    dragAcc = 0;
    if (g.y < HEADER_H) {
      if (g.x >= W - BACK_W - 8) return -1;
      continue;
    }
    if (g.y < HEADER_H + rows * ROW_H) {
      if (g.x >= W - ARROW_W) {
        int dir = (g.y < HEADER_H + rows * ROW_H / 2) ? -1 : 1;
        top = constrain(top + dir * (rows - 1), 0, maxTop);
        clampSelToView(); redraw = true;
      } else {
        int idx = top + (g.y - HEADER_H) / ROW_H;
        if (idx >= 0 && (size_t)idx < n) {
          sel = idx;
          drawList(title, n, label, sel, top, backLabel);
          if (!onOk) return sel;
          onOk(sel);
          redraw = true;
        }
      }
    }
  }
}

enum ConnResult { CONN_OK, CONN_WRONG_PASS, CONN_NOT_FOUND, CONN_TIMEOUT };

static volatile uint8_t s_lastReason = 0;

static const char* reasonText(uint8_t r) {
  switch (r) {
    case 2:   return "AUTH_EXPIRE";
    case 8:   return "ASSOC_LEAVE";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT (tipico de clave incorrecta)";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND (red no encontrada)";
    case 202: return "AUTH_FAIL (tipico de clave incorrecta)";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT (tipico de clave incorrecta)";
    case 205: return "CONNECTION_FAIL";
    default:  return "otro";
  }
}

static const char* statusText(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "?";
  }
}

static const char* authText(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return "ABIERTA";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENTERPRISE (no soportada aqui)";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "otra";
  }
}

static const char* connText(ConnResult r) {
  switch (r) {
    case CONN_WRONG_PASS: return "Clave incorrecta?";
    case CONN_NOT_FOUND:  return "Red no encontrada";
    case CONN_TIMEOUT:    return "Tiempo agotado";
    default:              return "";
  }
}

static ConnResult tryConnect(const String& ssid, const String& pass, uint32_t timeoutMs = 15000) {
  s_lastReason = 0;
  LOG("[WIFI] Intentando conectar a '%s' (clave de %u caracteres, timeout %lu ms)\n",
      ssid.c_str(), (unsigned)pass.length(), (unsigned long)timeoutMs);
  WiFi.disconnect();
  vTaskDelay(pdMS_TO_TICKS(100));
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t t = millis();
  int last = -1;
  while (millis() - t < timeoutMs) {
    wl_status_t st = WiFi.status();
    if ((int)st != last) {
      LOG("[WIFI] estado=%s (%d) a los %lu ms\n", statusText(st), (int)st, (unsigned long)(millis() - t));
      last = (int)st;
    }
    if (st == WL_CONNECTED) {
      LOG("[WIFI] CONECTADO. IP=%s  gateway=%s  RSSI=%d dBm\n",
          WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), (int)WiFi.RSSI());
      return CONN_OK;
    }
    if (st == WL_CONNECT_FAILED) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  uint8_t r = s_lastReason;
  WiFi.disconnect();
  ConnResult res = CONN_TIMEOUT;
  if (r == 201) res = CONN_NOT_FOUND;
  else if (r == 15 || r == 202 || r == 204 || r == 2) res = CONN_WRONG_PASS;
  LOG("[WIFI] FALLO: ultimo motivo=%u (%s) -> %s\n", (unsigned)r, reasonText(r),
      res == CONN_WRONG_PASS ? "contrasena incorrecta (probable)" :
      res == CONN_NOT_FOUND  ? "red no encontrada (¿es de 5 GHz? el ESP32 solo usa 2,4 GHz)" :
                               "timeout sin motivo claro");
  return res;
}

__attribute__((unused)) static bool enterPasswordPicker(const String& ssid, String& pw) {
  static const char CH[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()-_=+.,:;/?";
  const int N = sizeof(CH) - 1;
  int idx = 0; pw = "";
  tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Clave de: " + ssid, 6, 6, 2);
  tft.setTextPadding(tft.width());
  for (;;) {
    String cur = idx < N ? String(CH[idx]) : (idx == N ? "[BORRAR]" : "[LISTO]");
    tft.drawString(pw + "_", 6, 50, 4);
    tft.drawString("Letra: " + cur, 6, 110, 4);
    Key k = waitKey();
    if (k == Key::Down) idx = (idx + 1) % (N + 2);
    else if (k == Key::Up) idx = (idx + N + 1) % (N + 2);
    else if (k == Key::Back) { tft.setTextPadding(0); return false; }
    else if (k == Key::Ok) {
      if (idx < N) pw += CH[idx];
      else if (idx == N) { if (pw.length()) pw.remove(pw.length() - 1); }
      else { tft.setTextPadding(0); return true; }
    }
  }
}

#ifdef TOUCH_CS
static bool enterPasswordTouch(const String& ssid, String& pw) {
  static const char* ROWS[3][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._," },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._," },
    { "1234567890", "!@#$%^&*()", "[]{}<>/\\|?", "+=_-:;'\"~`" }
  };
  const int W = tft.width(), KY = 92, KH = 44, KW = W / 10;
  int layer = 0; pw = "";

  auto drawKeys = [&]() {
    tft.setTextPadding(0); tft.setTextDatum(MC_DATUM);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 10; c++) {
      int x = c * KW, y = KY + r * KH;
      tft.fillRoundRect(x + 2, y + 2, KW - 4, KH - 4, 5, TFT_DARKGREY);
      char s[2] = { ROWS[layer][r][c], 0 };
      tft.setTextColor(TFT_WHITE, TFT_DARKGREY); tft.drawString(s, x + KW / 2, y + KH / 2, 4);
    }
    int y = KY + 4 * KH;
    struct Btn { int x, w; const char* t; uint16_t col; };
    const Btn bs[4] = {
      { 0,   96,  layer == 0 ? "ABC" : (layer == 1 ? "#+=" : "abc"), TFT_DARKGREY },
      { 96,  192, "espacio",                                          TFT_DARKGREY },
      { 288, 96,  "Borrar",                                           TFT_MAROON   },
      { 384, 96,  "OK",                                               TFT_DARKGREEN }
    };
    for (const Btn& b : bs) {
      tft.fillRoundRect(b.x + 2, y + 2, b.w - 4, KH - 4, 5, b.col);
      tft.setTextColor(TFT_WHITE, b.col); tft.drawString(b.t, b.x + b.w / 2, y + KH / 2, 2);
    }
    tft.setTextDatum(TL_DATUM);
  };
  auto drawField = [&]() {
    tft.setTextPadding(0);
    tft.fillRoundRect(4, 40, W - 8, 44, 6, TFT_BLACK);
    tft.drawRoundRect(4, 40, W - 8, 44, 6, TFT_LIGHTGREY);
    String shown = pw; if (shown.length() > 28) shown = shown.substring(shown.length() - 28);
    tft.setTextDatum(ML_DATUM); tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(shown + "_", 12, 62, 4);
    tft.setTextDatum(TL_DATUM);
  };

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(ML_DATUM); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Clave de: " + ssid, 8, 18, 2);
  tft.fillRoundRect(W - 114, 4, 108, 28, 6, TFT_MAROON);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, TFT_MAROON); tft.drawString("Cancelar", W - 60, 18, 2);
  tft.setTextDatum(TL_DATUM);
  drawKeys(); drawField();

  for (;;) {
    if (s_wifiPhase) { pollSerial(); if (s_serialCreds) return false; }
    Gesture g = pollGesture();
    if (g.ev != TEv::Tap) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

    if (g.y < 36) { if (g.x >= W - 114) return false; continue; }
    if (g.y >= KY && g.y < KY + 4 * KH) {
      int c = constrain(g.x / KW, 0, 9), r = (g.y - KY) / KH;
      if (pw.length() < 63) pw += ROWS[layer][r][c];
      if (layer == 1) { layer = 0; drawKeys(); }
      drawField();
    } else if (g.y >= KY + 4 * KH && g.y < KY + 5 * KH) {
      if (g.x < 96)       { layer = (layer + 1) % 3; drawKeys(); }
      else if (g.x < 288) { if (pw.length() < 63) pw += ' '; drawField(); }
      else if (g.x < 384) { if (pw.length()) pw.remove(pw.length() - 1); drawField(); }
      else return true;
    }
  }
}
#endif

static bool enterPassword(const String& ssid, String& pw) {
#ifdef TOUCH_CS
  return enterPasswordTouch(ssid, pw);
#else
  return enterPasswordPicker(ssid, pw);
#endif
}

static void showConnected() {
  message("Wi-Fi conectado", WiFi.localIP().toString(), TFT_GREEN);
  vTaskDelay(pdMS_TO_TICKS(1500));
}

static void wifiConnected() {
  s_wifiPhase = false;
  String ip = WiFi.localIP().toString();
  LOG("[WIFI] Listo. Abri en el navegador:  http://%s   (o http://%s.local)\n", ip.c_str(), HOSTNAME);
  showConnected();
}

static bool attemptSave(const String& ssid, const String& pass) {
  message("Conectando a", ssid);
  ConnResult r = tryConnect(ssid, pass);
  if (r == CONN_OK) {
    g_wifiStore.save(ssid, pass);
    LOG("[WIFI] Credenciales guardadas en NVS\n");
    return true;
  }
  message(connText(r), ssid, TFT_RED);
  vTaskDelay(pdMS_TO_TICKS(1800));
  return false;
}

static int scanNetworks(std::vector<String>& names, std::vector<bool>& locked) {
  message("Buscando redes...");
  LOG("[WIFI] Escaneando redes...\n");
  names.clear(); locked.clear();
  int n = WiFi.scanNetworks();
  if (n < 0) LOG("[WIFI] El escaneo fallo (codigo %d)\n", (int)n);
  for (int i = 0; i < n; i++) {
    names.push_back(WiFi.SSID(i));
    locked.push_back(WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    LOG("[WIFI]  %2d) '%s'  canal %d  %d dBm  %s\n", i + 1, WiFi.SSID(i).c_str(),
        (int)WiFi.channel(i), (int)WiFi.RSSI(i), authText(WiFi.encryptionType(i)));
  }
  WiFi.scanDelete();
  LOG("[WIFI] Redes 2,4 GHz encontradas: %u\n", (unsigned)names.size());
  return (int)names.size();
}

static bool autoConnect() {
  if (g_wifiStore.count() == 0) {
    LOG("[WIFI] Wi-Fi SIN CONFIGURAR (no hay redes guardadas). Elegi una red en pantalla o usa el comando por Serial.\n");
    return false;
  }
  LOG("[WIFI] Redes guardadas: %u\n", (unsigned)g_wifiStore.count());
  std::vector<String> names; std::vector<bool> locked;
  scanNetworks(names, locked);

  for (size_t i = 0; i < g_wifiStore.count(); i++) {
    String ssid = g_wifiStore.ssid(i), pass = g_wifiStore.pass(i);
    bool visible = names.empty();
    for (const String& n : names) if (n == ssid) { visible = true; break; }
    if (!visible) { LOG("[WIFI] '%s' no esta al alcance\n", ssid.c_str()); continue; }
    message("Conectando a", ssid);
    if (tryConnect(ssid, pass) == CONN_OK) { g_wifiStore.save(ssid, pass); return true; }
  }
  LOG("[WIFI] No se pudo conectar a ninguna red guardada (no se borran). Paso a seleccion manual.\n");
  return false;
}

static bool connectNewNetwork(const char* backLabel) {
  if (s_serialCreds) {
    s_serialCreds = false;
    return attemptSave(s_serSsid, s_serPass);
  }

  std::vector<String> names; std::vector<bool> locked;
  if (scanNetworks(names, locked) == 0) {
    message("No se encontraron redes", "reintentando...", TFT_YELLOW);
    for (int i = 0; i < 20 && !s_serialCreds; i++) { pollSerial(); vTaskDelay(pdMS_TO_TICKS(100)); }
    return false;
  }

  int sel = listMenu("Elegi tu Wi-Fi", names.size(),
      [&](size_t i) { return String(locked[i] ? "* " : "  ") + names[i]; }, 0, nullptr, backLabel);
  if (s_serialCreds || sel < 0) return false;

  String pw = "";
  if (locked[sel] && !enterPassword(names[sel], pw)) return false;
  if (s_serialCreds) return false;
  return attemptSave(names[sel], pw);
}

static String currentSsid() {
  return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
}

static void switchTo(size_t idx) {
  String ssid = g_wifiStore.ssid(idx), pass = g_wifiStore.pass(idx);
  String prev = currentSsid();
  if (prev == ssid) {
    message("Ya conectado a", ssid, TFT_GREEN);
    vTaskDelay(pdMS_TO_TICKS(1200));
    return;
  }
  if (attemptSave(ssid, pass)) { showConnected(); return; }

  int p = prev.length() ? g_wifiStore.find(prev) : -1;
  if (p >= 0) {
    message("Volviendo a", prev);
    tryConnect(g_wifiStore.ssid(p), g_wifiStore.pass(p));
  }
}

static void forgetNetwork(size_t idx) {
  String ssid = g_wifiStore.ssid(idx);
  int c = listMenu("Olvidar " + ssid + "?", 2,
      [](size_t i) { return String(i == 0 ? "Si, olvidar" : "No"); }, 1, nullptr, "< Atras");
  if (c != 0) return;

  bool wasCurrent = currentSsid() == ssid;
  g_wifiStore.remove(idx);
  if (wasCurrent) WiFi.disconnect();
  LOG("[WIFI] Red olvidada: '%s'\n", ssid.c_str());
  message("Red olvidada", ssid, TFT_YELLOW);
  vTaskDelay(pdMS_TO_TICKS(1500));
}

static void savedNetworkMenu(size_t idx) {
  static const char* ACTIONS[] = { "Conectar", "Olvidar", "Volver" };
  int a = listMenu(g_wifiStore.ssid(idx), 3,
      [](size_t i) { return String(ACTIONS[i]); }, 0, nullptr, "< Atras");
  if (a == 0) switchTo(idx);
  else if (a == 1) forgetNetwork(idx);
}

static void wifiManager() {
  s_wifiPhase = true;
  for (;;) {
    String cur = currentSsid();
    int sel = listMenu("Wi-Fi", g_wifiStore.count() + 1,
        [&](size_t i) {
          if (i == 0) return String("+ Nueva red");
          const String& s = g_wifiStore.ssid(i - 1);
          return String(s == cur ? "> " : "  ") + s;
        }, 0, nullptr, "< Atras");

    if (sel < 0) {
      if (s_serialCreds && connectNewNetwork("< Atras")) showConnected();
      if (s_serialCreds) continue;
      break;
    }
    if (sel == 0) {
      if (connectNewNetwork("< Atras")) showConnected();
      continue;
    }
    savedNetworkMenu(sel - 1);
  }
  s_wifiPhase = false;
}

static void wifiFlow() {
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    s_lastReason = info.wifi_sta_disconnected.reason;
    LOG("[WIFI] Desconectado/rechazado. motivo=%u (%s)\n", (unsigned)s_lastReason, reasonText(s_lastReason));
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  s_wifiPhase = true;
  LOG("[WIFI] Modo STA. MAC=%s\n", WiFi.macAddress().c_str());
  LOG("[WIFI] Por Serial podes configurar con:  wifi NOMBRE_RED|CLAVE\n");

  g_wifiStore.load();
  if (autoConnect()) { wifiConnected(); return; }

  if (strlen(WIFI_SSID) > 0) {
    LOG("[WIFI] Probando credenciales de src/wifi_secrets.h para '%s'\n", WIFI_SSID);
    message("Conectando a", WIFI_SSID);
    if (tryConnect(String(WIFI_SSID), String(WIFI_PASS)) == CONN_OK) { wifiConnected(); return; }
    LOG("[WIFI] Fallaron las credenciales de wifi_secrets.h. Paso a seleccion manual.\n");
  }

  while (!connectNewNetwork("Reescanear")) {}
  wifiConnected();
}

#ifdef TOUCH_CS
static void drawTarget(int sx, int sy, uint16_t col) {
  tft.drawCircle(sx, sy, 16, col);
  tft.drawFastHLine(sx - 24, sy, 49, col);
  tft.drawFastVLine(sx, sy - 24, 49, col);
}

static void captureTouchPoint(int sx, int sy, const char* what, int idx, uint16_t& rx, uint16_t& ry) {
  const int W = tft.width(), H = tft.height();
  const uint16_t ZC = 250;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Calibracion " + String(idx) + "/4", W / 2, H / 2 - 24, 4);
  tft.drawString("Presiona el centro de la cruz y mantene", W / 2, H / 2 + 8, 2);
  tft.drawString("hasta que se ponga verde", W / 2, H / 2 + 28, 2);
  tft.setTextDatum(TL_DATUM);
  drawTarget(sx, sy, TFT_MAGENTA);

  uint32_t lastLog = 0;
  for (;;) {
    uint16_t z = tft.getTouchRawZ();
    if (millis() - lastLog > 1500) {
      lastLog = millis();
      LOG("[TOUCH] esperando punto %d/4 (%s)... z=%u (se necesita > %u)\n", idx, what, (unsigned)z, (unsigned)ZC);
    }
    if (z > ZC) {
      uint32_t sumX = 0, sumY = 0; int n = 0;
      uint16_t minX = 65535, maxX = 0, minY = 65535, maxY = 0;
      uint32_t t0 = millis();
      while (n < 12 && millis() - t0 < 1500) {
        if (tft.getTouchRawZ() > ZC) {
          uint16_t x = 0, y = 0; tft.getTouchRaw(&x, &y);
          sumX += x; sumY += y; n++;
          if (x < minX) minX = x; if (x > maxX) maxX = x;
          if (y < minY) minY = y; if (y > maxY) maxY = y;
        }
        delay(15);
      }
      if (n >= 12 && (maxX - minX) < 400 && (maxY - minY) < 400) {
        rx = (uint16_t)(sumX / n); ry = (uint16_t)(sumY / n);
        LOG("[TOUCH] punto %d/4 (%s): raw x=%u y=%u (dispersion %u / %u)\n", idx, what,
            (unsigned)rx, (unsigned)ry, (unsigned)(maxX - minX), (unsigned)(maxY - minY));
        drawTarget(sx, sy, TFT_GREEN);
        uint32_t tr = millis(), tOut = millis();
        while (millis() - tr < 400 && millis() - tOut < 5000) { if (tft.getTouchRawZ() > 100) tr = millis(); delay(10); }
        return;
      }
      LOG("[TOUCH] lectura inestable (muestras=%d), repeti el punto\n", n);
    }
    delay(10);
  }
}

static bool runCalibration(uint16_t cal[5]) {
  const int W = tft.width(), H = tft.height(), M = 40;
  const int px[4] = { M, W - M, M, W - M };
  const int py[4] = { M, M, H - M, H - M };
  const char* nm[4] = { "arriba-izquierda", "arriba-derecha", "abajo-izquierda", "abajo-derecha" };
  uint16_t rx[4] = {0}, ry[4] = {0};
  for (int i = 0; i < 4; i++) captureTouchPoint(px[i], py[i], nm[i], i + 1, rx[i], ry[i]);

  float dX = (float)(rx[1] + rx[3]) - (float)(rx[0] + rx[2]);
  float dY = (float)(ry[1] + ry[3]) - (float)(ry[0] + ry[2]);
  bool rotate = fabsf(dY) > fabsf(dX);
  const uint16_t* ax = rotate ? ry : rx;
  const uint16_t* ay = rotate ? rx : ry;

  float aL = (ax[0] + ax[2]) / 2.0f, aR = (ax[1] + ax[3]) / 2.0f;
  float aT = (ay[0] + ay[1]) / 2.0f, aB = (ay[2] + ay[3]) / 2.0f;
  float fx = (float)M / (W - 2 * M), fy = (float)M / (H - 2 * M);
  float a0 = aL - (aR - aL) * fx, aW = aR + (aR - aL) * fx;
  float b0 = aT - (aB - aT) * fy, bH = aB + (aB - aT) * fy;
  bool invX = aW < a0, invY = bH < b0;
  float xMin = invX ? aW : a0, xSpan = fabsf(aW - a0);
  float yMin = invY ? bH : b0, ySpan = fabsf(bH - b0);
  if (xMin < 0) xMin = 0;
  if (yMin < 0) yMin = 0;

  cal[0] = (uint16_t)xMin; cal[1] = (uint16_t)xSpan;
  cal[2] = (uint16_t)yMin; cal[3] = (uint16_t)ySpan;
  cal[4] = (rotate ? 1 : 0) | (invX ? 2 : 0) | (invY ? 4 : 0);
  LOG("[TOUCH] calculo: rotar=%d invertirX=%d invertirY=%d  x0=%u rangoX=%u y0=%u rangoY=%u\n",
      rotate, invX, invY, cal[0], cal[1], cal[2], cal[3]);
  return xSpan > 300 && ySpan > 300;
}

static void touchVerify() {
  const int W = tft.width();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Prueba del tactil (8 s)", W / 2, 30, 4);
  tft.drawString("Toca o arrastra: el punto verde debe quedar bajo tu dedo", W / 2, 64, 2);
  tft.setTextDatum(TL_DATUM);
  uint32_t t0 = millis();
  while (millis() - t0 < 8000) {
    uint16_t x = 0, y = 0;
    if (tft.getTouch(&x, &y, TOUCH_Z_MIN)) { tft.fillCircle(x, y, 6, TFT_GREEN); LOG("[TOUCH] prueba: x=%u y=%u\n", (unsigned)x, (unsigned)y); }
    delay(10);
  }
}

static void touchInit() {
  uint16_t cal[5] = { 0 };
  Preferences p; p.begin("touch", false);
  bool have = p.isKey("cal_r1") && (p.getBytesLength("cal_r1") == sizeof(cal)) &&
              (p.getBytes("cal_r1", cal, sizeof(cal)) == sizeof(cal));

  message("Tactil", "Mantene la pantalla tocada ahora para recalibrar");
  int held = 0;
  for (int i = 0; i < 15; i++) { if (tft.getTouchRawZ() > TOUCH_Z_MIN) held++; delay(100); }
  bool force = held >= 4;

  if (!have || force) {
    LOG("[TOUCH] %s: calibrando\n", force ? "recalibracion pedida" : "sin calibracion guardada");
    tft.setTouch(cal);
    while (!runCalibration(cal)) {
      LOG("[TOUCH] calibracion invalida, se repite\n");
      message("Calibracion invalida", "Presiona bien cada cruz", TFT_YELLOW);
      delay(1500);
    }
    p.putBytes("cal_r1", cal, sizeof(cal));
    LOG("[TOUCH] calibracion guardada: %u %u %u %u %u\n", cal[0], cal[1], cal[2], cal[3], cal[4]);
    tft.setTouch(cal);
    touchVerify();
  } else {
    LOG("[TOUCH] calibracion cargada: %u %u %u %u %u\n", cal[0], cal[1], cal[2], cal[3], cal[4]);
    tft.setTouch(cal);
  }
  p.end();
  tft.fillScreen(TFT_BLACK);
}
#endif

void uiInit() {
#if ENCODER_ENABLED
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  pinMode(PIN_BACK, INPUT_PULLUP);
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  enc.attachHalfQuad(PIN_ENC_A, PIN_ENC_B);
  enc.clearCount();
#endif
  tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);
  LOG("[UI] TFT inicializado %dx%d. Encoder: %s\n", tft.width(), tft.height(), ENCODER_ENABLED ? "habilitado" : "deshabilitado");
#ifdef TOUCH_CS
  touchInit();
#endif
}

void uiTask(void*) {
  LOG("[UI] uiTask iniciada en core %d\n", (int)xPortGetCoreID());
  wifiFlow();
  xEventGroupSetBits(g_sysEvents, EVT_WIFI_READY);
  int lastCat = 1;

  for (;;) {
    String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("sin Wi-Fi");
    int cat = listMenu("Categorias  " + ip, 2 + g_m3u.groupCount(),
        [](size_t i) {
          if (i == 0) return String("Wi-Fi: ") + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("sin conexion"));
          if (i == 1) return String("Todos (") + g_m3u.count() + ")";
          return g_m3u.groupName(i - 2) + " (" + g_m3u.groupSize(i - 2) + ")";
        }, lastCat);
    if (cat < 0) continue;
    if (cat == 0) { wifiManager(); continue; }
    lastCat = cat;

    std::vector<uint16_t> view;
    g_m3u.filter(cat == 1 ? -1 : cat - 2, view);

    auto label = [&](size_t i) { Channel c; return g_m3u.get(view[i], c) ? c.name : String("?"); };
    listMenu(cat == 1 ? String("Todos") : g_m3u.groupName(cat - 2), view.size(), label, 0,
        [&](size_t i) {
          PlayEvent ev{view[i]};
          xQueueOverwrite(g_playQueue, &ev);
          s_now = String("> ") + label(i);
        });
  }
}
