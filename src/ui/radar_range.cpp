#include "ui/radar_range.h"

#include "ui/radar_theme.h"

#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsRunwaysKey[] = "showRwys";
constexpr uint8_t kDefaultRangeIndex = 0;  

Preferences s_prefs;
uint8_t s_range_index = kDefaultRangeIndex;
bool s_use_miles = false;
bool s_show_runways = true;

void saveRangeIndex() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_range_index = static_cast<uint8_t>((s_range_index) % kRangePresetCount);
  s_prefs.putUChar(kPrefsRangeKey, s_range_index);
  s_prefs.end();
}

void saveUseMiles() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsMilesKey, s_use_miles);
  s_prefs.end();
}

void saveShowRunways() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRunwaysKey, s_show_runways);
  s_prefs.end();
}

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  // WiFiManager checkbox submits its value= attribute ("T", or "F" if we prefilled F).
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = s_prefs.getUChar(kPrefsRangeKey, kDefaultRangeIndex);
  s_range_index =
      (saved < kRangePresetCount) ? saved : kDefaultRangeIndex;
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, false);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
  s_prefs.end();
}

void rangeNext() {
  s_range_index = static_cast<uint8_t>((s_range_index + 1) % kRangePresetCount);
  saveRangeIndex();
}

const RangePreset& rangeCurrent() { return kRangePresets[s_range_index]; }

uint8_t rangeIndex() { return s_range_index; }

float fetchRadiusKm() {
  const float outer_km = rangeCurrent().outer_km;
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return outer_km * (screen_r_px / static_cast<float>(kGridOuterRadius));
}

bool useMiles() { return s_use_miles; }

bool showRunways() { return s_show_runways; }

void saveMilesFromPortal(const char* checkbox_value) {
  s_use_miles = portalCheckboxChecked(checkbox_value);
  saveUseMiles();
  Serial.printf("Distance units: %s\n", s_use_miles ? "miles" : "km");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  s_show_runways = portalCheckboxChecked(checkbox_value);
  saveShowRunways();
  Serial.printf("Runway overlay: %s\n", s_show_runways ? "on" : "off");
}

bool parseIndex(const char* text, u8_t* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const long v = strtol(text, &end, 10);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  if (v < 256)
    *out = v;
  return true;
}

void saveRangeFromPortal(const char* index_value) {
  uint8_t v;
  Serial.printf("selected range = %s\n", index_value);
  if (parseIndex(index_value, &v)) {
    s_range_index = v;
    saveRangeIndex();

    char scale_label[12];
    radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
    Serial.printf("Range index: %d, %s\n", s_range_index, scale_label);
  }
}

void getRangeIndexes(char* buff, size_t len) {
  char delim[3] = "";
  char labelstr[20];
  for (int indx = 0; indx < kRangePresetCount; indx++) {
    float ring3_km = kRangePresets[indx].ring3_km;
    if (s_use_miles) {
      const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMi));
      snprintf(labelstr, sizeof(labelstr), "%s%d: %dmi", delim, indx, mi);
    } else {
      const int km = static_cast<int>(lroundf(ring3_km));
      snprintf(labelstr, sizeof(labelstr), "%s%d: %dkm", delim, indx, km);
    }
    sprintf(delim, ", ");
    if (strlen(buff) + strlen(labelstr) < len) {
      buff = strcat(buff, labelstr);
    }
  }
}

void getRangeHtml(const char* id, char* buff, size_t len) {
  snprintf(buff, len, "");
  char line[200];
  sprintf(line, "<label for='%s_ddl'>Select display range:</label>\n", id);
  buff = strcat(buff, line);
  sprintf(line, "<select id='%s_ddl' name='%s_ddl' onchange=\"document.getElementById('%s').value = this.value\">\n", id, id, id);
  buff = strcat(buff, line);
  for (int indx = 0; indx < kRangePresetCount; indx++) {
    char miStr[10];
    char kmStr[10];
    char selected[20] = "\0";
    char optionStr[100] = "\0";
    float ring3_km = kRangePresets[indx].ring3_km;
    sprintf(kmStr, "%.1f", ring3_km);
    char* p = strstr(kmStr, ".0");
    if (p) *p = 0x0;
    sprintf(kmStr, "%s km", kmStr);
    sprintf(miStr, "%.1f", ring3_km / kKmPerMi);
    p = strstr(miStr, ".0");
    if (p) *p = 0x0;
    sprintf(miStr, "%s mi", miStr);
    char* first = kmStr;
    char* second = miStr;
    if (s_use_miles) {
      first = miStr;
      second = kmStr;
    }
    if (indx == rangeIndex())
      sprintf(selected, "selected");
    sprintf(optionStr, "<option value='%d' %s >%s (%s)</option>\n", indx, selected, first, second);
    buff = strcat(buff, optionStr);
  }
  buff = strcat(buff, "</select>\n");
  buff = strcat(buff, "<script>\n");
  sprintf(line, "document.getElementById('%s').value = '%d';\n", id, rangeIndex());
  buff = strcat(buff, line);
  sprintf(line, "document.querySelector(\"[for='%s']\").hidden = true;\n", id);
  buff = strcat(buff, line);
  sprintf(line, "document.getElementById('%s').hidden = true;\n", id);
  buff = strcat(buff, line);
  buff = strcat(buff, "</script>\n");
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  if (use_miles) {
    // const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMi));
    // snprintf(buf, len, "%dmi", mi);
    sprintf(buf, "%.1f", ring3_km / kKmPerMi);
    char* p = strstr(buf, ".0");
    if (p) *p = 0x0;
    sprintf(buf, "%s mi", buf);
  } else {
    // const int km = static_cast<int>(lroundf(ring3_km));
    // snprintf(buf, len, "%dkm", km);
    sprintf(buf, "%.1f", ring3_km);
    char* p = strstr(buf, ".0");
    if (p) *p = 0x0;
    sprintf(buf, "%s km", buf);
  }
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void unitsReset() {
  s_range_index =  0;
  s_use_miles = false;
  s_show_runways = true;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
