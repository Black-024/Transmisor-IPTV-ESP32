#pragma once
#include <Arduino.h>
#include <vector>

class WifiStore {
public:
  void   load();
  size_t count() const { return _nets.size(); }
  const String& ssid(size_t i) const { return _nets[i].ssid; }
  const String& pass(size_t i) const { return _nets[i].pass; }
  int    find(const String& ssid) const;
  void   save(const String& ssid, const String& pass);
  void   remove(size_t i);

private:
  struct Net { String ssid; String pass; };
  void persist();

  std::vector<Net> _nets;
};

extern WifiStore g_wifiStore;
