#pragma once

#ifdef USE_ESP32

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#include "decode.h"

namespace esphome {
namespace homewhiz {

// 16-bit characteristics, identical across all HomeWhiz appliances (plan §3.1).
static const uint16_t HOMEWHIZ_NOTIFY_CHAR = 0xAC02;  // state stream
static const uint16_t HOMEWHIZ_WRITE_CHAR = 0xAC01;   // handshake + commands

// Handshake (plan §3.2) and command frame prefix (plan §3.6).
static const uint8_t HOMEWHIZ_HANDSHAKE[8] = {0x02, 0x04, 0x00, 0x04,
                                              0x00, 0x1A, 0x01, 0x03};

class HomeWhiz : public ble_client::BLEClientNode, public Component {
 public:
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void set_service_uuid(const std::string &uuid) { this->service_uuid_raw_ = uuid; }

  // True once connected + handshaken to the appliance. Used to suppress BLE
  // scan logging once we have a live link (scan only while unconnected).
  bool is_connected() const { return this->handshaken_; }

#ifdef USE_SENSOR
  // factor > 0 forces a numeric reading (raw byte & 0x7F) * factor, even when the
  // field is modelled as an enum (e.g. spin: byte 12 -> 1200 rpm with factor 100).
  void register_sensor(const std::string &key, sensor::Sensor *s, float factor = 0.0f) {
    this->sensors_.push_back({key, s, factor, 0.0f, false});
  }
#endif
#ifdef USE_TEXT_SENSOR
  void register_text_sensor(const std::string &key, text_sensor::TextSensor *s) {
    this->text_sensors_.push_back({key, s, std::string(), false});
  }
#endif
#ifdef USE_BINARY_SENSOR
  void register_binary_sensor(const std::string &key, binary_sensor::BinarySensor *s) {
    this->binary_sensors_.push_back({key, s, false, false});
  }
  // Optional hub-level status entity (device_class connectivity): true once
  // connected + handshaken to the appliance. Not a mapping.h field — it reflects
  // the BLE link, so HA can mark stale data unavailable when the appliance is off
  // or out of range.
  void set_connected_binary_sensor(binary_sensor::BinarySensor *s) {
    this->connected_binary_sensor_ = s;
  }
#endif

  // Stretch goal (plan §3.6 / T-B4): write value V to appliance index I. Guarded
  // — only call from an explicit user action.
  void send_command(uint8_t index, uint8_t value);
  // Convenience: write by mapping.h WRITE_* key (e.g. "STATE").
  bool send_command_key(const std::string &key, uint8_t value);

 protected:
  void handle_frame_(const uint8_t *frame, size_t len);
  void log_gatt_hint_();
  void write_char_(uint16_t char_handle, const uint8_t *data, size_t len,
                   bool with_response);

  std::string service_uuid_raw_;
  esp32_ble_tracker::ESPBTUUID service_uuid_;
  esp32_ble_tracker::ESPBTUUID notify_uuid_{esp32_ble_tracker::ESPBTUUID::from_uint16(
      HOMEWHIZ_NOTIFY_CHAR)};
  esp32_ble_tracker::ESPBTUUID write_uuid_{esp32_ble_tracker::ESPBTUUID::from_uint16(
      HOMEWHIZ_WRITE_CHAR)};

  uint16_t notify_handle_{0};
  uint16_t write_handle_{0};
  bool handshaken_{false};
  // millis() when the BLE link opened (0 = not connected). Used only to report
  // connection lifetime in the DISCONNECT log.
  uint32_t connect_time_ms_{0};

  MessageAccumulator accumulator_;

  // Each entry caches the last value it published so handle_frame_ can skip
  // republishing unchanged fields: the appliance streams the full state on every
  // frame (several a second), so without this every entity re-publishes
  // constantly — spamming the API/logs and blocking the BLE stack. `published`
  // guards the first frame, when there's no previous value to compare against.
#ifdef USE_SENSOR
  struct SensorEntry {
    std::string key;
    sensor::Sensor *sensor;
    float factor;  // 0 = decode by field kind; >0 = force numeric raw*factor
    float last;
    bool published;
  };
  std::vector<SensorEntry> sensors_;
#endif
#ifdef USE_TEXT_SENSOR
  struct TextSensorEntry {
    std::string key;
    text_sensor::TextSensor *sensor;
    std::string last;
    bool published;
  };
  std::vector<TextSensorEntry> text_sensors_;
#endif
#ifdef USE_BINARY_SENSOR
  struct BinarySensorEntry {
    std::string key;
    binary_sensor::BinarySensor *sensor;
    bool last;
    bool published;
  };
  std::vector<BinarySensorEntry> binary_sensors_;
  binary_sensor::BinarySensor *connected_binary_sensor_{nullptr};
  bool connected_published_{false};
  bool connected_state_{false};
#endif
};

}  // namespace homewhiz
}  // namespace esphome

#endif  // USE_ESP32
