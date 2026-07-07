// Pure, transport-independent HomeWhiz decode core.
//
// Deliberately free of any ESPHome/ESP-IDF dependency so it can be unit-tested
// on the host (see tests/test_decode.cpp). It is driven entirely by the
// generated mapping.h — there are NO appliance field names or numeric offsets
// here. Adding a new appliance means regenerating mapping.h, never editing this
// file. See plan §3.7 / §6-T-B1.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mapping.h"

namespace esphome {
namespace homewhiz {

// §3.4 decode rule: every value byte carries a flag in its high bit.
inline uint8_t hw_value(const uint8_t *data, size_t len, uint8_t index) {
  // Bounds-checked like upstream safe_get(): out of range -> 0.
  if (index >= len) return 0;
  return data[index] & 0x7F;
}

enum DecodedType : uint8_t { DECODED_NONE = 0, DECODED_TEXT, DECODED_NUMBER };

struct DecodedField {
  DecodedType type = DECODED_NONE;
  float number = 0.0f;      // valid when type == DECODED_NUMBER
  const char *text = nullptr;  // valid when type == DECODED_TEXT (enum key / flag)
};

// Decode a single field descriptor against a reassembled frame.
// Returns false when the field cannot be decoded from this frame (e.g. an enum
// byte whose value isn't in the table) — callers should then not publish.
inline bool hw_decode(const FieldDesc &d, const uint8_t *data, size_t len,
                      DecodedField &out) {
  switch (d.kind) {
    case KIND_ENUM: {
      uint8_t v = hw_value(data, len, d.index);
      for (uint8_t i = 0; i < d.enum_count; i++) {
        if (d.enums[i].value == v) {
          out.type = DECODED_TEXT;
          out.text = d.enums[i].key;
          return true;
        }
      }
      return false;  // unknown enum value for this frame
    }
    case KIND_NUMERIC: {
      out.type = DECODED_NUMBER;
      out.number = static_cast<float>(hw_value(data, len, d.index)) * d.factor;
      return true;
    }
    case KIND_PROGRESS: {
      out.type = DECODED_NUMBER;
      out.number = static_cast<float>(hw_value(data, len, d.index)) * 60.0f +
                   static_cast<float>(hw_value(data, len, d.index2));
      return true;
    }
    case KIND_FLAG: {
      // Warning/flag bits are read from the RAW byte (upstream
      // BooleanBitmaskControl), not the & 0x7F value.
      uint8_t raw = d.index < len ? data[d.index] : 0;
      out.type = DECODED_TEXT;
      out.text = ((raw >> d.index2) & 0x01) ? "on" : "off";
      return true;
    }
  }
  return false;
}

inline const FieldDesc *hw_find_field(const char *key) {
  for (uint8_t i = 0; i < HW_FIELD_COUNT; i++) {
    if (std::strcmp(HW_FIELDS[i].key, key) == 0) return &HW_FIELDS[i];
  }
  return nullptr;
}

inline const WriteDesc *hw_find_write(const char *key) {
  for (uint8_t i = 0; i < HW_WRITE_COUNT; i++) {
    if (HW_WRITES[i].key != nullptr && std::strcmp(HW_WRITES[i].key, key) == 0) {
      return &HW_WRITES[i];
    }
  }
  return nullptr;
}

// §3.3 fragment reassembly. State arrives as two BLE notifications, each with a
// 7-byte header; byte 4 is the fragment index. Faithful port of upstream
// MessageAccumulator. Notifications shorter than 10 bytes are ignored by the
// caller before reaching here.
class MessageAccumulator {
 public:
  // Feed one notification. Returns pointer+len of a complete frame when the
  // second fragment arrives, else {nullptr, 0}. The returned buffer is owned by
  // the accumulator and valid until the next feed().
  const uint8_t *feed(const uint8_t *msg, size_t len, size_t &out_len) {
    out_len = 0;
    if (len < 7) return nullptr;
    uint8_t index = msg[4];
    if (index == 0) {
      buf_len_ = len - 7;
      std::memcpy(buf_, msg + 7, buf_len_ > sizeof(buf_) ? sizeof(buf_) : buf_len_);
      if (buf_len_ > sizeof(buf_)) buf_len_ = sizeof(buf_);
      expected_ = 1;
      return nullptr;
    }
    if (index == 1 && expected_ == 1) {
      size_t add = len - 7;
      if (buf_len_ + add > sizeof(buf_)) add = sizeof(buf_) - buf_len_;
      std::memcpy(buf_ + buf_len_, msg + 7, add);
      buf_len_ += add;
      expected_ = 0;
      out_len = buf_len_;
      return buf_;
    }
    // Unexpected sequence: reset so we don't get permanently stuck.
    expected_ = 0;
    buf_len_ = 0;
    return nullptr;
  }

 private:
  // Frames observed so far are ~77 bytes; 256 leaves generous headroom for
  // larger appliance types without dynamic allocation.
  uint8_t buf_[256];
  size_t buf_len_ = 0;
  uint8_t expected_ = 0;
};

}  // namespace homewhiz
}  // namespace esphome
