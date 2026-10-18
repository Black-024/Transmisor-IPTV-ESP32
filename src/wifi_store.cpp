#include "wifi_store.h"
#include <Preferences.h>
#include "config.h"

WifiStore g_wifiStore;

static String keyOf(char prefix, size_t i) { return String(prefix) + String((unsigned)i); }

void WifiStore::load() {
  _nets.clear();
  Preferences p; p.begin("wifi", false);

  size_t n = p.getUChar("n", 0);
  if (n > WIFI_MAX_SAVED) n = WIFI_MAX_SAVED;
  for (size_t i = 0; i < n; i++) {
    String s = p.getString(keyOf('s', i).c_str(), "");
    if (s.length()) _nets.push_back({s, p.getString(keyOf('p', i).c_str(), "")});
  }

  if (p.isKey("ssid")) {
    String s = p.getString("ssid", ""), pw = p.getString("pass", "");
    p.remove("ssid");
    if (p.isKey("pass")) p.remove("pass");
    p.end();
    if (s.length()) save(s, pw);
    return;
  }
  p.end();
}

int WifiStore::find(const String& ssid) const {
  for (size_t i = 0; i < _nets.size(); i++) if (_nets[i].ssid == ssid) return (int)i;
  return -1;
}

void WifiStore::save(const String& ssid, const String& pass) {
  int i = find(ssid);
  if (i == 0 && _nets[0].pass == pass) return;
  if (i >= 0) _nets.erase(_nets.begin() + i);
  Net net{ssid, pass};
  _nets.insert(_nets.begin(), net);
  if (_nets.size() > WIFI_MAX_SAVED) _nets.erase(_nets.begin() + WIFI_MAX_SAVED, _nets.end());
  persist();
}

void WifiStore::remove(size_t i) {
  if (i >= _nets.size()) return;
  _nets.erase(_nets.begin() + i);
  persist();
}

void WifiStore::persist() {
  Preferences p; p.begin("wifi", false);
  p.putUChar("n", (uint8_t)_nets.size());
  for (size_t i = 0; i < _nets.size(); i++) {
    p.putString(keyOf('s', i).c_str(), _nets[i].ssid);
    p.putString(keyOf('p', i).c_str(), _nets[i].pass);
  }
  for (size_t i = _nets.size(); i < WIFI_MAX_SAVED; i++) {
    String ks = keyOf('s', i), kp = keyOf('p', i);
    if (p.isKey(ks.c_str())) p.remove(ks.c_str());
    if (p.isKey(kp.c_str())) p.remove(kp.c_str());
  }
  p.end();
}
