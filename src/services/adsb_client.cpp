#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstring>

#include "config.h"

namespace services::adsb {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/%.7f/lon/%.7f/dist/%.1f";
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 1000 * 5;
constexpr float kKmPerNm = 1.852f;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

bool readJsonResponse(HTTPClient& http, JsonDocument& doc) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return false;
  }

  const int content_length = http.getSize();
  Serial.printf("adsb: content_length=%d\n", content_length);

  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    if (stream->available() > 0) {
      const DeserializationError err = deserializeJson(doc, *stream);
      if (err == DeserializationError::Ok) {
        return true;
      }
      if (err != DeserializationError::IncompleteInput) {
        Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
        return false;
      }
    }
    if (!http.connected() && stream->available() <= 0) {
      break;
    }
    delay(5);
  }

  Serial.println("adsb: incomplete JSON response");
  return false;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  if (plane["emergency"].is<const char*>()) {
    const char* s = plane["emergency"].as<const char*>();
    if (strcmp(s, "none") != 0) {
      char tmp[sizeof(ac->callsign)];
      snprintf(tmp, sizeof(ac->callsign), "!%s", ac->callsign);  
      strncpy(ac->callsign, tmp, sizeof(ac->callsign));
    }
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  float dist_nm = kmToNauticalMiles(fetch_radius_km);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(2500);

  while (true){
    unsigned long start = millis();

    char url[256];
    snprintf(url, sizeof(url), kApiBase, center_lat, center_lon, dist_nm);

    Serial.printf("adsb: fetchUpdate url=%s\n", url);

    HTTPClient http;
    if (!http.begin(client, url)) {
      Serial.println("adsb: http.begin failed");
      http.end();
      return false;
    }

    http.setTimeout(kRequestTimeoutMs * 2);
    const int code = performGetWithPoll(http);
    if (code != HTTP_CODE_OK) {
      Serial.printf("adsb: HTTP %d\n", code);
      http.end();
      return false;
    }

    JsonDocument doc;
    if (readJsonResponse(http, doc)) {
      http.end();
      JsonArray ac = doc["ac"].as<JsonArray>();
      if (ac.isNull()) {
        s_aircraft_count = 0;
        return true;
      }

      size_t n = 0;
      for (JsonObject plane : ac) {
        if (n >= kMaxAircraft) {
          break;
        }
        if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
          continue;
        }
        if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
          continue;
        }

        s_aircraft[n].lat = plane["lat"].as<float>();
        s_aircraft[n].lon = plane["lon"].as<float>();
        s_aircraft[n].nose_deg = pickNoseHeading(plane);
        s_aircraft[n].track_deg = pickTrackHeading(plane);
        s_aircraft[n].gs_knots = pickGroundSpeed(plane);
        fillTagFields(&s_aircraft[n], plane);
        ++n;
      }

      s_aircraft_count = n;
      Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
      return true;
    }

    Serial.println("adsb: empty response");
    http.end();
    if (dist_nm <= 6 * kKmPerNm) {
      dist_nm -= 1 * kKmPerNm;
    }
    else
      dist_nm -= 5 * kKmPerNm;
    if (dist_nm <= 0) {
      Serial.println("adsb: fetchUpdate failed, no data");
      return false;
    }

    while (millis() - start < 1100) {
      pollNetwork();
      delay(5);
    }
  }
}

}  // namespace services::adsb
