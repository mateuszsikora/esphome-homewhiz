// Host-side unit test for the table-driven decode core + fragment reassembly.
//
// Runs without ESPHome or hardware. It generates the washer mapping.h from the
// upstream fixture (see tests/run.sh), includes it via decode.h, and decodes the
// verified reference frame from plan §3.5.
//
// Build/run: tests/run.sh
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "decode.h"

using namespace esphome::homewhiz;

static int failures = 0;

#define CHECK(cond, msg)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      std::printf("FAIL: %s\n", msg);                           \
      failures++;                                               \
    } else {                                                    \
      std::printf("ok:   %s\n", msg);                           \
    }                                                           \
  } while (0)

static std::vector<uint8_t> from_hex(const std::string &hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back((uint8_t) std::stoul(hex.substr(i, 2), nullptr, 16));
  return out;
}

// Verified reference frame (§3.5): state=on, cottons, 30 C, 137 min remaining.
static const char *FRAME_HEX =
    "002f4a45a10100000000000000000000000000000000000000000200000000000000000a011e0c"
    "0000000080021102110000000000000000000000000000000100000000000001070000000000";

static bool decode_key(const uint8_t *f, size_t len, const char *key,
                       DecodedField &out) {
  const FieldDesc *d = hw_find_field(key);
  if (d == nullptr) return false;
  return hw_decode(*d, f, len, out);
}

int main() {
  std::vector<uint8_t> frame = from_hex(FRAME_HEX);
  const uint8_t *f = frame.data();
  size_t len = frame.size();
  std::printf("frame length: %zu bytes\n", len);
  CHECK(len == 77, "reassembled frame is 77 bytes");

  DecodedField v;

  // --- fields the config models as proper enums / progress: must decode ---
  CHECK(decode_key(f, len, "STATE", v) && v.type == DECODED_TEXT &&
            std::strcmp(v.text, "DEVICE_STATE_ON") == 0,
        "STATE decodes to DEVICE_STATE_ON");

  CHECK(decode_key(f, len, "WASHER_PROGRAM", v) && v.type == DECODED_TEXT &&
            std::strcmp(v.text, "PROGRAM_COTTONS") == 0,
        "WASHER_PROGRAM decodes to PROGRAM_COTTONS");

  CHECK(decode_key(f, len, "WASHER_TEMPERATURE", v) && v.type == DECODED_TEXT &&
            std::strcmp(v.text, "TEMPERATURE_30") == 0,
        "WASHER_TEMPERATURE decodes to TEMPERATURE_30");

  CHECK(decode_key(f, len, "WASHER_REMAINING", v) && v.type == DECODED_NUMBER &&
            v.number == 137.0f,
        "WASHER_REMAINING decodes to 137 minutes");

  CHECK(decode_key(f, len, "WASHER_DURATION", v) && v.type == DECODED_NUMBER &&
            v.number == 137.0f,
        "WASHER_DURATION decodes to 137 minutes");

  // --- documented wrinkle: spin is a sparse enum {no_spin, rinse_hold} in the
  // config; byte 38 == 12 (the "1200 rpm" from plan §3.5) is NOT an enumerated
  // value, so a config-driven decode reports unknown. This asserts the real
  // config-driven behaviour, not the plan's numeric interpretation. ---
  CHECK(!decode_key(f, len, "WASHER_SPIN", v),
        "WASHER_SPIN(12) is unknown (config models spin as sparse enum)");

  // --- fragment reassembly (§3.3): split the frame into two notifications with
  // 7-byte headers (byte 4 = fragment index) and confirm it reassembles. ---
  MessageAccumulator acc;
  size_t out_len = 0;
  const size_t split = 40;
  std::vector<uint8_t> frag0(7, 0), frag1(7, 0);
  frag0[4] = 0;
  frag1[4] = 1;
  frag0.insert(frag0.end(), frame.begin(), frame.begin() + split);
  frag1.insert(frag1.end(), frame.begin() + split, frame.end());

  CHECK(acc.feed(frag0.data(), frag0.size(), out_len) == nullptr,
        "first fragment yields no frame yet");
  const uint8_t *reassembled = acc.feed(frag1.data(), frag1.size(), out_len);
  CHECK(reassembled != nullptr && out_len == len &&
            std::memcmp(reassembled, f, len) == 0,
        "second fragment reassembles the original frame");

  std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", failures);
  return failures ? 1 : 0;
}
