#ifdef USE_ESP32

#include "homewhiz.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"  // millis()

namespace esphome {
namespace homewhiz {

static const char *const TAG = "homewhiz";

// ---------------------------------------------------------------------------
// NOTE (plan §6-T-B5): the ESP-IDF/ESPHome BLE accessor names below
// (get_gattc_if, get_conn_id, get_remote_bda, get_characteristic, node_state,
// ESPBTUUID) have drifted across ESPHome releases. Pin the ESPHome version in
// README; if the build fails, adjust these names — the logic is unchanged.
// ---------------------------------------------------------------------------

void HomeWhiz::loop() {
#ifdef USE_BINARY_SENSOR
  // Publish the connectivity status on change (and once at boot, so HA shows
  // "disconnected" rather than "unknown" before the first link).
  if (this->connected_binary_sensor_ != nullptr) {
    bool connected = this->is_connected();
    if (!this->connected_published_ || connected != this->connected_state_) {
      this->connected_binary_sensor_->publish_state(connected);
      this->connected_state_ = connected;
      this->connected_published_ = true;
    }
  }
#endif
}

void HomeWhiz::dump_config() {
  // dump_config() also runs when a log viewer subscribes with dump_config set,
  // i.e. a new `esphome logs` session or opening Logs (not an automatic
  // reconnect). The logger keeps no history, so re-arm the GATT table: a viewer
  // that started after the first failure still gets it.
  this->gatt_table_logged_ = false;
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
  for (auto &e : this->text_sensors_) {
    const FieldDesc *d = hw_find_field(e.key.c_str());
    check("Text sensor", e.key, d != nullptr && d->kind == KIND_ENUM);
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &e : this->binary_sensors_) {
    const FieldDesc *d = hw_find_field(e.key.c_str());
    check("Binary sensor", e.key, d != nullptr && d->kind == KIND_FLAG);
  }
#endif
}

void HomeWhiz::log_gatt_hint_() {
  // Tell apart "service_uuid matched nothing" from "service matched, but it
  // lacks AC01/AC02" — the two need different fixes.
  bool service_found = this->parent()->get_service(this->service_uuid_) != nullptr;
  const char *where = this->gatt_table_logged_ ? " logged earlier" : " below";
  // A log client's automatic reconnect doesn't re-run dump_config(), so point
  // at restarting the viewer, not just waiting for it to reconnect.
  const char *again = this->gatt_table_logged_
                          ? " To see the table again, restart `esphome logs` or "
                            "reopen Logs."
                          : "";
  if (!service_found) {
    ESP_LOGE(TAG,
             "service_uuid '%s' matches no service on the appliance. Use the "
             "service that lists both 0xAC01 and 0xAC02 in the GATT table%s.%s",
             this->service_uuid_raw_.c_str(), where, again);
  } else {
    ESP_LOGE(TAG,
             "AC01/AC02 not found under service '%s'. If the GATT table%s lists "
             "them under this service, please open an issue with this log; "
             "otherwise use the service that lists both.%s",
             this->service_uuid_raw_.c_str(), where, again);
  }
  // The appliance drops the link without a handshake and ble_client reconnects
  // every ~10 s, so dump the table once (re-armed by dump_config()) rather than
  // on every attempt.
  if (!this->gatt_table_logged_) {
    this->log_gatt_table_();
    this->gatt_table_logged_ = true;
  }
}

void HomeWhiz::log_gatt_table_() {
  // Dump the peer's GATT table straight from the ESP-IDF cache (the same source
  // the ESPHome Bluetooth proxy reads). ESPHome's own service/characteristic
  // dump is only compiled in at logger level VERBOSE; this one is logged as
  // warnings, so it shows at the default level.
  // Loops are capped so a misbehaving stack can't spin here forever. Past the
  // last entry ESP-IDF returns ESP_GATT_INVALID_OFFSET, which ends a loop.
  static constexpr uint16_t MAX_ENTRIES = 32;
  auto gattc_if = this->parent()->get_gattc_if();
  auto conn_id = this->parent()->get_conn_id();
  char uuid_buf[esp32_ble_tracker::UUID_STR_LEN];
  ESP_LOGW(TAG, "GATT table of the appliance (16-bit UUIDs go into service_uuid "
                "without the 0x, e.g. \"AC00\"):");
  uint16_t s = 0;
  for (; s < MAX_ENTRIES; s++) {
    esp_gattc_service_elem_t svc;
    uint16_t svc_count = 1;
    auto status =
        esp_ble_gattc_get_service(gattc_if, conn_id, nullptr, &svc, &svc_count, s);
    if (status != ESP_GATT_OK || svc_count == 0) {
      if (s == 0)
        ESP_LOGW(TAG, "  (no services in the ESP-IDF cache, status=%d)", status);
      break;
    }
    ESP_LOGW(TAG, "  service %s, handles 0x%04x-0x%04x",
             esp32_ble_tracker::ESPBTUUID::from_uuid(svc.uuid).to_str(uuid_buf),
             svc.start_handle, svc.end_handle);
    uint16_t c = 0;
    for (; c < MAX_ENTRIES; c++) {
      esp_gattc_char_elem_t chr;
      uint16_t chr_count = 1;
      auto chr_status = esp_ble_gattc_get_all_char(
          gattc_if, conn_id, svc.start_handle, svc.end_handle, &chr, &chr_count, c);
      if (chr_status != ESP_GATT_OK || chr_count == 0) {
        if (c == 0 && chr_status != ESP_GATT_INVALID_OFFSET &&
            chr_status != ESP_GATT_NOT_FOUND)
          ESP_LOGW(TAG, "    (characteristics unavailable, status=%d)", chr_status);
        break;
      }
      ESP_LOGW(TAG, "    characteristic %s, handle 0x%04x, properties 0x%02x",
               esp32_ble_tracker::ESPBTUUID::from_uuid(chr.uuid).to_str(uuid_buf),
               chr.char_handle, chr.properties);
    }
    if (c == MAX_ENTRIES)
      ESP_LOGW(TAG, "    ... truncated after %u characteristics", (unsigned) MAX_ENTRIES);
  }
  if (s == MAX_ENTRIES)
    ESP_LOGW(TAG, "  ... truncated after %u services", (unsigned) MAX_ENTRIES);
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
      if (param->open.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "GATT open failed: status=%d", param->open.status);
      } else {
        // Mark when the link opened, so DISCONNECT can report how long it held.
        // Only on success, so connect_time_ms_ stays 0 for a link that never
        // opened (keeps the "held" figure honest on a later disconnect).
        this->connect_time_ms_ = millis();
      }
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
      if (esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(),
                                            this->parent()->get_remote_bda(),
                                            this->notify_handle_) != ESP_OK)
        ESP_LOGW(TAG, "register_for_notify on 0x%04x could not be sent",
                 this->notify_handle_);
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.handle != this->notify_handle_)
        break;
      // Without a working subscription the state stream never reaches us, so
      // don't handshake (and don't report "connected").
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "register_for_notify on 0x%04x failed: status=%d",
                 param->reg_for_notify.handle, param->reg_for_notify.status);
        break;
      }
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
      bool saw_extra_fragment = false;
      const uint8_t *frame = this->accumulator_.feed(
          param->notify.value, param->notify.value_len, out_len, &saw_extra_fragment);
      if (saw_extra_fragment)
        ESP_LOGW(TAG,
                 "BLE state frame carries a fragment index >= 2: this appliance's "
                 "frames span more than the two fragments the HomeWhiz protocol "
                 "uses, so they cannot be reassembled and are dropped. Not seen on "
                 "any known appliance — please open an issue with your model.");
      if (frame != nullptr)
        this->handle_frame_(frame, out_len);
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      // Log WHY the link dropped + how long it held, to tell benign causes apart
      // from RF instability. reason is the HCI error code (esp_gatt_conn_reason_t):
      //   0x08 = supervision timeout   -> RF/range: real instability
      //   0x13 = peer closed the link  -> appliance powered off / dropped us
      //   0x16 = local host closed      -> our stack closed it (e.g. slot reused)
      //   0x3e = failed to establish    -> connection never really came up
      // handshaked=no means it dropped before we finished the handshake.
      float held_s =
          this->connect_time_ms_ != 0 ? (millis() - this->connect_time_ms_) / 1000.0f : 0.0f;
      ESP_LOGW(TAG, "Disconnected: reason=0x%02x, held %.1fs, handshaked=%s",
               param->disconnect.reason, held_s, this->handshaken_ ? "yes" : "no");
      this->connect_time_ms_ = 0;
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
  // The appliance re-sends the whole state on every frame, so publish only the
  // fields that actually changed since we last published them (see the entry
  // structs). This keeps a steady state from re-emitting ~24 entities several
  // times a second — which spammed the API and blocked the BLE stack.
  DecodedField v;
#ifdef USE_SENSOR
  for (auto &e : this->sensors_) {
    const FieldDesc *d = hw_find_field(e.key.c_str());
    if (d == nullptr)
      continue;
    float value;
    if (e.factor > 0.0f) {
      // Forced numeric: raw value * factor (e.g. enum-modelled spin -> rpm).
      value = hw_value(frame, len, d->index) * e.factor;
    } else if (hw_decode(*d, frame, len, v) && v.type == DECODED_NUMBER) {
      value = v.number;
    } else {
      continue;
    }
    if (!e.published || value != e.last) {
      e.sensor->publish_state(value);
      e.last = value;
      e.published = true;
    }
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto &e : this->text_sensors_) {
    const FieldDesc *d = hw_find_field(e.key.c_str());
    if (d == nullptr)
      continue;
    if (hw_decode(*d, frame, len, v) && v.type == DECODED_TEXT &&
        (!e.published || e.last != v.text)) {
      e.sensor->publish_state(v.text);
      e.last = v.text;  // copies the string; v.text may point at scratch
      e.published = true;
    }
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &e : this->binary_sensors_) {
    const FieldDesc *d = hw_find_field(e.key.c_str());
    if (d == nullptr)
      continue;
    if (hw_decode(*d, frame, len, v) && v.type == DECODED_BOOL &&
        (!e.published || e.last != v.boolean)) {
      e.sensor->publish_state(v.boolean);
      e.last = v.boolean;
      e.published = true;
    }
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
