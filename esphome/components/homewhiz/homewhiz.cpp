#ifdef USE_ESP32

#include "homewhiz.h"
#include "esphome/core/log.h"

namespace esphome {
namespace homewhiz {

static const char *const TAG = "homewhiz";

// ---------------------------------------------------------------------------
// NOTE (plan §6-T-B5): the ESP-IDF/ESPHome BLE accessor names below
// (get_gattc_if, get_conn_id, get_remote_bda, get_characteristic, node_state,
// ESPBTUUID) have drifted across ESPHome releases. Pin the ESPHome version in
// README; if the build fails, adjust these names — the logic is unchanged.
// ---------------------------------------------------------------------------

void HomeWhiz::dump_config() {
  ESP_LOGCONFIG(TAG, "HomeWhiz:");
  ESP_LOGCONFIG(TAG, "  Service UUID: %s", this->service_uuid_raw_.c_str());
  ESP_LOGCONFIG(TAG, "  Mapping: %u fields, %u write targets", (unsigned) HW_FIELD_COUNT,
                (unsigned) HW_WRITE_COUNT);
  // Log each registered key and check it exists with a compatible kind for the
  // platform it's wired to (sensor<-numeric/progress, text_sensor<-enum,
  // binary_sensor<-flag). Mismatches would silently never publish otherwise.
  auto check = [](const char *platform, const std::string &key, bool ok_kind) {
    const FieldDesc *d = hw_find_field(key.c_str());
    if (d == nullptr)
      ESP_LOGE(TAG, "  %s key '%s' not found in mapping.h", platform, key.c_str());
    else if (!ok_kind)
      ESP_LOGE(TAG, "  %s key '%s' has incompatible kind %u for this platform",
               platform, key.c_str(), (unsigned) d->kind);
    else
      ESP_LOGCONFIG(TAG, "  %s key: %s", platform, key.c_str());
  };
#ifdef USE_SENSOR
  for (auto &e : this->sensors_) {
    const FieldDesc *d = hw_find_field(e.key.c_str());
    // A factor override makes any field numeric, so enum is fine in that case.
    check("Sensor", e.key,
          d != nullptr && (e.factor > 0.0f || d->kind == KIND_NUMERIC ||
                           d->kind == KIND_PROGRESS));
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto &kv : this->text_sensors_) {
    const FieldDesc *d = hw_find_field(kv.first.c_str());
    check("Text sensor", kv.first, d != nullptr && d->kind == KIND_ENUM);
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &kv : this->binary_sensors_) {
    const FieldDesc *d = hw_find_field(kv.first.c_str());
    check("Binary sensor", kv.first, d != nullptr && d->kind == KIND_FLAG);
  }
#endif
}

void HomeWhiz::log_gatt_hint_() {
  ESP_LOGE(TAG,
           "AC01/AC02 not found under service '%s'. The correct service_uuid is "
           "the one whose characteristics include both 0xAC01 and 0xAC02 — see "
           "the 'Service UUID:' / 'Characteristic UUID:' lines that ble_client "
           "logged for this device above, or scan with nRF Connect (plan §3.1).",
           this->service_uuid_raw_.c_str());
}

void HomeWhiz::write_char_(uint16_t char_handle, const uint8_t *data, size_t len,
                           bool with_response) {
  auto status = esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(), char_handle,
      len, const_cast<uint8_t *>(data),
      with_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
      ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK)
    ESP_LOGW(TAG, "write_char to handle 0x%04x failed, status=%d", char_handle,
             status);
}

void HomeWhiz::gattc_event_handler(esp_gattc_cb_event_t event,
                                   esp_gatt_if_t /*gattc_if*/,
                                   esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      // Request a larger MTU so the ~77-byte state frame arrives in two
      // fragments rather than many (plan §3.3). ble_client usually negotiates
      // this already; requesting is idempotent.
      esp_ble_gattc_send_mtu_req(this->parent()->get_gattc_if(),
                                 this->parent()->get_conn_id());
      this->handshaken_ = false;
      this->notify_handle_ = 0;
      this->write_handle_ = 0;
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      this->service_uuid_ = esp32_ble_tracker::ESPBTUUID::from_raw(this->service_uuid_raw_);
      auto *notify_chr =
          this->parent()->get_characteristic(this->service_uuid_, this->notify_uuid_);
      auto *write_chr =
          this->parent()->get_characteristic(this->service_uuid_, this->write_uuid_);
      if (notify_chr == nullptr || write_chr == nullptr) {
        this->log_gatt_hint_();
        break;
      }
      this->notify_handle_ = notify_chr->handle;
      this->write_handle_ = write_chr->handle;
      ESP_LOGI(TAG, "Found AC02 notify=0x%04x, AC01 write=0x%04x", this->notify_handle_,
               this->write_handle_);
      // Subscribe to the state stream (plan §3.2).
      esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(),
                                        this->parent()->get_remote_bda(),
                                        this->notify_handle_);
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.handle != this->notify_handle_)
        break;
      // Handshake: write 8 bytes to AC01 WITHOUT response. The appliance streams
      // nothing until it receives this (plan §3.2).
      this->write_char_(this->write_handle_, HOMEWHIZ_HANDSHAKE,
                        sizeof(HOMEWHIZ_HANDSHAKE), /*with_response=*/false);
      this->handshaken_ = true;
      ESP_LOGI(TAG, "Handshake sent");
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->notify_handle_)
        break;
      // Ignore notifications shorter than 10 bytes (plan §3.3).
      if (param->notify.value_len < 10)
        break;
      size_t out_len = 0;
      const uint8_t *frame =
          this->accumulator_.feed(param->notify.value, param->notify.value_len, out_len);
      if (frame != nullptr)
        this->handle_frame_(frame, out_len);
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      this->handshaken_ = false;
      this->notify_handle_ = 0;
      this->write_handle_ = 0;
      // Drop any half-received frame so it can't bridge across the reconnect.
      this->accumulator_.reset();
      break;
    }
    default:
      break;
  }
}

void HomeWhiz::handle_frame_(const uint8_t *frame, size_t len) {
  DecodedField v;
#ifdef USE_SENSOR
  for (auto &e : this->sensors_) {
    const FieldDesc *d = hw_find_field(e.key.c_str());
    if (d == nullptr)
      continue;
    if (e.factor > 0.0f) {
      // Forced numeric: raw value * factor (e.g. enum-modelled spin -> rpm).
      e.sensor->publish_state(hw_value(frame, len, d->index) * e.factor);
    } else if (hw_decode(*d, frame, len, v) && v.type == DECODED_NUMBER) {
      e.sensor->publish_state(v.number);
    }
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto &kv : this->text_sensors_) {
    const FieldDesc *d = hw_find_field(kv.first.c_str());
    if (d == nullptr)
      continue;
    if (hw_decode(*d, frame, len, v) && v.type == DECODED_TEXT)
      kv.second->publish_state(v.text);
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &kv : this->binary_sensors_) {
    const FieldDesc *d = hw_find_field(kv.first.c_str());
    if (d == nullptr)
      continue;
    if (hw_decode(*d, frame, len, v) && v.type == DECODED_BOOL)
      kv.second->publish_state(v.boolean);
  }
#endif
}

void HomeWhiz::send_command(uint8_t index, uint8_t value) {
  if (!this->handshaken_ || this->write_handle_ == 0) {
    ESP_LOGW(TAG, "send_command ignored: not connected/handshaken");
    return;
  }
  // Command frame (plan §3.6): 02 04 00 04 00 <index> 01 <value>
  const uint8_t cmd[8] = {0x02, 0x04, 0x00, 0x04, 0x00, index, 0x01, value};
  this->write_char_(this->write_handle_, cmd, sizeof(cmd), /*with_response=*/false);
  ESP_LOGI(TAG, "send_command index=%u value=%u", index, value);
}

bool HomeWhiz::send_command_key(const std::string &key, uint8_t value) {
  const WriteDesc *w = hw_find_write(key.c_str());
  if (w == nullptr) {
    ESP_LOGW(TAG, "send_command_key: '%s' has no write target in mapping.h",
             key.c_str());
    return false;
  }
  this->send_command(w->index, value);
  return true;
}

}  // namespace homewhiz
}  // namespace esphome

#endif  // USE_ESP32
