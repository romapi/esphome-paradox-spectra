#include "paradox_spectra.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include <cstring>

namespace esphome {
namespace paradox_spectra {

static const char *const TAG = "paradox_spectra";
static const uint8_t FRAME_LEN = 37;
static const size_t BUF_CAP = 256;

// Checksum = sum of bytes[0..35] mod 256 == byte[36] (PAI common.calculate_checksum).
bool ParadoxSpectra::checksum_ok_(const uint8_t *frame) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < FRAME_LEN - 1; i++)
    sum += frame[i];
  return static_cast<uint8_t>(sum & 0xFF) == frame[FRAME_LEN - 1];
}

void ParadoxSpectra::loop() {
  // Drain the UART into the ring buffer.
  uint8_t b;
  while (this->available()) {
    this->read_byte(&b);
    this->buf_.push_back(b);
    if (this->buf_.size() > BUF_CAP)
      this->buf_.erase(this->buf_.begin());  // overflow guard
  }
  // Extract every valid 37-byte frame at the front; resync by dropping one byte
  // when the checksum fails (no guaranteed start delimiter on a passive tap).
  while (this->buf_.size() >= FRAME_LEN) {
    const uint8_t *f = this->buf_.data();
    if (ParadoxSpectra::checksum_ok_(f)) {
      this->handle_frame_(f);
      this->buf_.erase(this->buf_.begin(), this->buf_.begin() + FRAME_LEN);
    } else {
      this->buf_.erase(this->buf_.begin());
    }
  }
}

void ParadoxSpectra::handle_frame_(const uint8_t *f) {
  const uint8_t cmd = f[0] >> 4;

  if (this->debug_frames_) {
    ESP_LOGD(TAG, "frame cmd=0x%X: %s", cmd,
             format_hex_pretty(f, FRAME_LEN).c_str());
  }

  // Passive decoder: only LiveEvent (0xE) is handled. Status/EEPROM responses appear
  // only when a client polls the panel — logged as debug here, not decoded.
  if (cmd != 0xE)
    return;

  const uint8_t major = f[7];      // event group
  const uint8_t minor = f[8];      // zone / user / sub-code (depends on event)
  const uint8_t partition = f[9] + 1;

  // timestamp: bytes[1..6] = century, year, month, day, hour, minute.
  char ts[32];
  snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u",
           (unsigned) (f[1] * 100 + f[2]), f[3], f[4], f[5], f[6]);

  // label: bytes[15..30], ASCII null/space padded.
  char label[17];
  std::memcpy(label, f + 15, 16);
  label[16] = '\0';
  for (int i = 15; i >= 0; i--) {
    if (label[i] == ' ' || label[i] == '\0')
      label[i] = '\0';
    else
      break;
  }

  // Sensor publishing (only zone open/close + fire). Unmapped zones -> discovery log.
  if (major == 0 || major == 1) {
    const bool open = (major == 1);
    auto it = this->open_.find(minor);
    if (it != this->open_.end())
      it->second->publish_state(open);
    else
      this->log_unknown_(minor, label, "open", open ? "OPEN" : "closed");
  } else if (major == 37 || major == 39) {
    const bool fire = (major == 37);
    auto it = this->fire_.find(minor);
    if (it != this->fire_.end())
      it->second->publish_state(fire);
    else
      this->log_unknown_(minor, label, "fire", fire ? "ALARM" : "restore");
  }

  // Unified log of EVERY decoded event: timestamp + name + sub-code + label.
  // Alarms/fire/trouble -> WARN; everything else -> INFO if log_all_events, else DEBUG.
  const char *name = ParadoxSpectra::event_name_(major);
  const bool alert = (major == 36 || major == 37 || major == 38 || major == 39 ||
                      major == 40 || major == 42 || major == 44);
  if (alert) {
    ESP_LOGW(TAG, "[%s] %s (maj=%u min=%u p%u) '%s'", ts, name, major, minor, partition, label);
  } else if (this->log_all_events_) {
    ESP_LOGI(TAG, "[%s] %s (maj=%u min=%u p%u) '%s'", ts, name, major, minor, partition, label);
  } else {
    ESP_LOGD(TAG, "[%s] %s (maj=%u min=%u p%u) '%s'", ts, name, major, minor, partition, label);
  }

  if (this->last_event_ != nullptr) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s %s z%u '%s'", ts, name, minor, label);
    this->last_event_->publish_state(msg);
  }
}

// event.major -> human name (Paradox Spectra event groups; from PAI event_map).
const char *ParadoxSpectra::event_name_(uint8_t major) {
  switch (major) {
    case 0: return "zone closed";
    case 1: return "zone open";
    case 2: return "partition status";
    case 6: return "non-reportable";
    case 7: return "remote access";
    case 12: return "wireless zone cold start";
    case 13: return "wireless output cold start";
    case 14: return "bypass programming";
    case 16: return "wireless smoke maintenance";
    case 17: return "delay zone alarm tx";
    case 24: return "fire delay started";
    case 26: return "upload/download access";
    case 27: return "bus module add/remove";
    case 29: return "ARM (user)";
    case 30: return "special arming";
    case 31: return "DISARM (user)";
    case 32: return "disarm after alarm";
    case 33: return "alarm cancelled";
    case 34: return "special disarming";
    case 35: return "zone bypassed";
    case 36: return "ZONE IN ALARM";
    case 37: return "FIRE ALARM";
    case 38: return "zone alarm restore";
    case 39: return "fire alarm restore";
    case 40: return "SPECIAL ALARM";
    case 41: return "zone shutdown";
    case 42: return "ZONE TAMPER";
    case 43: return "zone tamper restore";
    case 44: return "NEW TROUBLE";
    case 45: return "trouble restored";
    case 46: return "bus module trouble";
    case 47: return "bus module restore";
    case 48: return "special";
    case 49: return "zone low battery";
    case 50: return "zone low battery restore";
    case 51: return "zone supervision trouble";
    case 52: return "zone supervision restore";
    case 64: return "system status";
    default: return "event";
  }
}

void ParadoxSpectra::log_unknown_(uint8_t zone, const char *label, const char *kind,
                                  const char *state) {
  // WARN once per newly-seen zone (so discovery stands out), DEBUG on repeats.
  const bool first = this->seen_unknown_.insert(zone).second;
  if (first) {
    ESP_LOGW(TAG,
             "DISCOVERED zone %u '%s' (%s %s) — not in config; add:  - { zone: %u, "
             "name: \"%s\"%s }",
             zone, label, kind, state, zone, label,
             (std::strcmp(kind, "fire") == 0) ? ", type: fire, device_class: smoke" : "");
  } else {
    ESP_LOGD(TAG, "unregistered zone %u '%s' (%s %s)", zone, label, kind, state);
  }
}

void ParadoxSpectra::dump_config() {
  ESP_LOGCONFIG(TAG, "Paradox Spectra decoder (passive LiveEvent)");
  ESP_LOGCONFIG(TAG, "  debug_frames: %s", YESNO(this->debug_frames_));
  ESP_LOGCONFIG(TAG, "  bound zones: open=%u fire=%u", (unsigned) this->open_.size(),
                (unsigned) this->fire_.size());
}

}  // namespace paradox_spectra
}  // namespace esphome
