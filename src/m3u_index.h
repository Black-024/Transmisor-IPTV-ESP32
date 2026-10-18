#pragma once
#include <Arduino.h>
#include <FS.h>
#include <vector>
#include <freertos/semphr.h>

struct Channel { String name; String group; String url; };

class M3UIndex {
public:
  bool   build(const char* path);
  size_t count() const        { return _refs.size(); }
  size_t groupCount() const   { return _groups.size(); }
  const String& groupName(size_t g) const { return _groups[g]; }
  uint16_t groupSize(size_t g) const      { return _groupSizes[g]; }
  bool   get(size_t index, Channel& out);
  void   filter(int group, std::vector<uint16_t>& out) const;

private:
  struct __attribute__((packed)) Ref { uint32_t offset; uint8_t group; };
  uint8_t groupId(const char* name);

  std::vector<Ref>      _refs;
  std::vector<String>   _groups;
  std::vector<uint16_t> _groupSizes;
  File _f;
  SemaphoreHandle_t _mtx = nullptr;
};

extern M3UIndex g_m3u;
