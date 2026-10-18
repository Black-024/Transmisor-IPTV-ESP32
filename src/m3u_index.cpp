#include "m3u_index.h"
#include <LittleFS.h>
#include "config.h"

M3UIndex g_m3u;

static int readLine(File& f, char* buf, size_t n) {
  size_t i = 0; bool any = false;
  while (f.available()) {
    int c = f.read(); any = true;
    if (c == '\n') break;
    if (c == '\r') continue;
    if (i < n - 1) buf[i++] = (char)c;
  }
  buf[i] = 0;
  return any ? (int)i : -1;
}

static bool extractAttr(const char* line, const char* key, char* out, size_t n) {
  char pat[24]; snprintf(pat, sizeof(pat), "%s=\"", key);
  const char* p = strstr(line, pat); if (!p) return false;
  p += strlen(pat);
  const char* e = strchr(p, '"');    if (!e) return false;
  size_t l = (size_t)(e - p); if (l > n - 1) l = n - 1;
  memcpy(out, p, l); out[l] = 0; return true;
}

static String extractName(const char* line) {
  const char* q = strrchr(line, '"');
  const char* c = strchr(q ? q : line, ',');
  if (!c) return String("(sin nombre)");
  c++; while (*c == ' ') c++;
  return String(c);
}

uint8_t M3UIndex::groupId(const char* name) {
  if (!name || !*name) name = "Sin categoria";
  for (size_t i = 0; i < _groups.size(); i++) if (_groups[i].equals(name)) return (uint8_t)i;
  if (_groups.size() >= MAX_GROUPS) return (uint8_t)(_groups.size() - 1);
  _groups.push_back(String(name)); _groupSizes.push_back(0);
  return (uint8_t)(_groups.size() - 1);
}

bool M3UIndex::build(const char* path) {
  if (!_mtx) _mtx = xSemaphoreCreateMutex();
  _f = LittleFS.open(path, "r");
  if (!_f) return false;
  _refs.clear(); _groups.clear(); _groupSizes.clear(); _refs.reserve(512);

  char line[M3U_LINE_MAX], grp[64];
  uint32_t pendingOff = 0; uint8_t pendingGrp = 0; bool pending = false;

  for (;;) {
    uint32_t pos = _f.position();
    int len = readLine(_f, line, sizeof(line));
    if (len < 0) break;
    if (len == 0) continue;
    if (strncmp(line, "#EXTINF", 7) == 0) {
      grp[0] = 0; extractAttr(line, "group-title", grp, sizeof(grp));
      pendingGrp = groupId(grp); pendingOff = pos; pending = true;
    } else if (line[0] != '#' && pending) {
      if (_refs.size() < MAX_CHANNELS) {
        _refs.push_back({pendingOff, pendingGrp});
        _groupSizes[pendingGrp]++;
      }
      pending = false;
    }
    if ((_refs.size() & 0xFF) == 0) vTaskDelay(1);
  }
  return !_refs.empty();
}

bool M3UIndex::get(size_t i, Channel& out) {
  if (i >= _refs.size()) return false;
  char line[M3U_LINE_MAX];
  xSemaphoreTake(_mtx, portMAX_DELAY);
  _f.seek(_refs[i].offset);
  readLine(_f, line, sizeof(line));
  out.name  = extractName(line);
  out.group = _groups[_refs[i].group];
  out.url   = "";
  while (readLine(_f, line, sizeof(line)) >= 0) {
    if (!line[0] || line[0] == '#') continue;
    out.url = line; break;
  }
  xSemaphoreGive(_mtx);
  return out.url.length() > 0;
}

void M3UIndex::filter(int group, std::vector<uint16_t>& out) const {
  out.clear(); out.reserve(group < 0 ? _refs.size() : _groupSizes[group]);
  for (size_t i = 0; i < _refs.size(); i++)
    if (group < 0 || _refs[i].group == group) out.push_back((uint16_t)i);
}
