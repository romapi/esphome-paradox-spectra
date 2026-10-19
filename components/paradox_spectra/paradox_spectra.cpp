#include "paradox_spectra.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include <cstring>

namespace esphome {
namespace paradox_spectra {

static const char *const TAG = "paradox_spectra";
static const uint8_t FRAME_LEN = 37;
static const size_t BUF_CAP = 256;
static const uint16_t RAM_BASE = 0x8000;  // status_base1 (PAI mem_map)
static const uint8_t NUM_POLL_BLOCKS = 2;  // 0 = voltages+zone_open+zone_fire, 1 = partitions

// Checksum = sum of bytes[0..35] mod 256 == byte[36] (PAI common.calculate_checksum).
bool ParadoxSpectra::checksum_ok_(const uint8_t *frame) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < FRAME_LEN - 1; i++)
    sum += frame[i];
  return static_cast<uint8_t>(sum & 0xFF) == frame[FRAME_LEN - 1];
}

void ParadoxSpectra::loop() {
  uint8_t b;
  while (this->available()) {
    this->read_byte(&b);
    this->buf_.push_back(b);
    if (this->buf_.size() > BUF_CAP)
      this->buf_.erase(this->buf_.begin());
  }
  while (this->buf_.size() >= FRAME_LEN) {
    const uint8_t *f = this->buf_.data();
    if (ParadoxSpectra::checksum_ok_(f)) {
      this->handle_frame_(f);
      this->buf_.erase(this->buf_.begin(), this->buf_.begin() + FRAME_LEN);
    } else {
      this->buf_.erase(this->buf_.begin());
    }
  }

  // Active poll (only if enabled). Login once settings are known, then poll RAM.
  if (this->poll_interval_ == 0)
    return;
  const uint32_t now = millis();
  if (!this->logged_in_) {
    if (this->have_settings_ && now - this->last_login_try_ > 2000) {
      this->last_login_try_ = now;
      this->send_login_();
    }
    return;
  }
  if (now - this->last_poll_ >= this->poll_interval_) {
    this->last_poll_ = now;
    this->send_read_status_(this->poll_block_);
    this->poll_block_ = (this->poll_block_ + 1) % NUM_POLL_BLOCKS;
  }
}

void ParadoxSpectra::handle_frame_(const uint8_t *f) {
  const uint8_t cmd = f[0] >> 4;

  if (this->debug_frames_)
    ESP_LOGD(TAG, "frame cmd=0x%X: %s", cmd, format_hex_pretty(f, FRAME_LEN).c_str());

  // --- Session / status frames (active poll) ---
  if (f[0] == 0x00) {  // panel greeting: InitializeCommunication from the panel
    this->product_id_ = f[4];
    this->fw_[0] = f[5];
    this->fw_[1] = f[6];
    this->fw_[2] = f[7];
    this->panel_id_ = (uint16_t) (f[8] << 8) | f[9];
    this->have_settings_ = true;
    ESP_LOGI(TAG, "Panel greeting: product_id=0x%02X fw=%u.%u.%u panel_id=0x%04X",
             this->product_id_, this->fw_[0], this->fw_[1], this->fw_[2], this->panel_id_);
    if (this->poll_interval_ > 0 && !this->logged_in_)
      this->send_login_();
    return;
  }
  if (cmd == 0x1) {  // 0x10 InitializeCommunicationResponse -> login OK
    this->logged_in_ = true;
    this->last_poll_ = 0;  // poll immediately
    ESP_LOGI(TAG, "Login OK (authenticated)");
    return;
  }
  if (cmd == 0x7) {  // 0x7x error / 0x70 close
    this->logged_in_ = false;
    ESP_LOGW(TAG, "Login failed / connection closed (0x%02X) — wrong PC password?", f[0]);
    return;
  }
  if (cmd == 0x5 && f[2] == 0x80) {  // ReadStatusResponse: validation 0x80, block in f[3]
    this->handle_status_(f + 4, f[3]);  // 32-byte RAM payload
    return;
  }
  if (cmd != 0xE)
    return;  // other command responses: ignore

  // --- LiveEvent (0xE) ---
  const uint8_t major = f[7];
  const uint8_t minor = f[8];
  const uint8_t partition = f[9] + 1;

  char ts[32];
  snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u",
           (unsigned) (f[1] * 100 + f[2]), f[3], f[4], f[5], f[6]);

  char label[17];
  std::memcpy(label, f + 15, 16);
  label[16] = '\0';
  for (int i = 15; i >= 0; i--) {
    if (label[i] == ' ' || label[i] == '\0')
      label[i] = '\0';
    else
      break;
  }

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

  const char *name = ParadoxSpectra::event_name_(major);
  const bool alert = (major == 36 || major == 37 || major == 38 || major == 39 ||
                      major == 40 || major == 42 || major == 44);
  if (alert)
    ESP_LOGW(TAG, "[%s] %s (maj=%u min=%u p%u) '%s'", ts, name, major, minor, partition, label);
  else if (this->log_all_events_)
    ESP_LOGI(TAG, "[%s] %s (maj=%u min=%u p%u) '%s'", ts, name, major, minor, partition, label);
  else
    ESP_LOGD(TAG, "[%s] %s (maj=%u min=%u p%u) '%s'", ts, name, major, minor, partition, label);

  if (this->last_event_ != nullptr) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s %s z%u '%s'", ts, name, minor, label);
    this->last_event_->publish_state(msg);
  }
}

// ---- Active poll: TX ----

void ParadoxSpectra::send_frame_(uint8_t *f) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < FRAME_LEN - 1; i++)
    sum += f[i];
  f[FRAME_LEN - 1] = static_cast<uint8_t>(sum & 0xFF);
  this->write_array(f, FRAME_LEN);
}

void ParadoxSpectra::send_login_() {
  // InitializeCommunication (command 0x00). Layout per PAI spectra_magellan parsers.
  uint8_t f[FRAME_LEN] = {0};
  f[0] = 0x00;                 // command
  f[4] = this->product_id_;    // echo panel's product_id
  f[5] = this->fw_[0];         // firmware version/revision/build
  f[6] = this->fw_[1];
  f[7] = this->fw_[2];
  f[8] = (uint8_t) (this->panel_id_ >> 8);  // panel_id (big-endian)
  f[9] = (uint8_t) (this->panel_id_ & 0xFF);
  // pc_password: right-align to 4 hex chars, parse to 2 bytes (PAI a2b_hex(pw.zfill(4))).
  char pw[4];
  {
    const std::string &p = this->pc_password_;
    std::string s = p.size() >= 4 ? p.substr(p.size() - 4) : std::string(4 - p.size(), '0') + p;
    for (int i = 0; i < 4; i++) pw[i] = s[i];
  }
  auto hexnib = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  f[10] = (hexnib(pw[0]) << 4) | hexnib(pw[1]);
  f[11] = (hexnib(pw[2]) << 4) | hexnib(pw[3]);
  f[12] = 0x19;   // _not_used1 (PAI sets this)
  f[33] = 0x02;   // source_id (PAI uses 0x02)
  ESP_LOGI(TAG, "Installer login (product_id=0x%02X)", this->product_id_);
  this->send_frame_(f);
}

void ParadoxSpectra::send_read_status_(uint8_t block) {
  // ReadEEPROM (command 0x50) at address RAM_BASE + block.
  uint8_t f[FRAME_LEN] = {0};
  const uint16_t addr = RAM_BASE + block;
  f[0] = 0x50;
  f[2] = (uint8_t) (addr >> 8);
  f[3] = (uint8_t) (addr & 0xFF);
  f[33] = 0x01;   // source_id default
  this->send_frame_(f);
}

// ---- RAM status decode ----

void ParadoxSpectra::publish_zone_bitmap_(const uint8_t *b4) {
  for (auto &kv : this->open_) {
    const uint8_t zone = kv.first;        // 1-based
    const uint8_t byte = (zone - 1) / 8;  // 0..3
    const uint8_t bit = (zone - 1) % 8;
    if (byte < 4)
      kv.second->publish_state((b4[byte] >> bit) & 0x01);
  }
}

void ParadoxSpectra::handle_status_(const uint8_t *d, uint8_t block) {
  // d = 32-byte RAM payload. Offsets per RAMDataParserMap (see docs/paradox-serial-protocol).
  if (block == 0) {
    // troubles[0..4], date[5..10], vdc[11], battery[12], dc[13], rf_noise[14],
    // zone_open[15..18], zone_tamper[19..22], pgm/module_tamper[23..26], zone_fire[27..30]
    const float vdc = d[11] * (20.3f - 1.4f) / 255.0f + 1.4f;
    const float battery = d[12] * 22.8f / 255.0f;
    const float dc = d[13] * 22.8f / 255.0f;
    if (this->supply_voltage_ != nullptr) this->supply_voltage_->publish_state(vdc);
    if (this->battery_voltage_ != nullptr) this->battery_voltage_->publish_state(battery);
    if (this->dc_voltage_ != nullptr) this->dc_voltage_->publish_state(dc);
    if (this->rf_noise_ != nullptr) this->rf_noise_->publish_state(d[14]);
    this->publish_zone_bitmap_(d + 15);  // authoritative current open/closed
    // zone_fire bitmap (d+27): set fire sensors from current state.
    for (auto &kv : this->fire_) {
      const uint8_t z = kv.first;
      if ((z - 1) / 8 < 4)
        kv.second->publish_state((d[27 + (z - 1) / 8] >> ((z - 1) % 8)) & 0x01);
    }
    ESP_LOGD(TAG, "RAM0: vdc=%.1f batt=%.1f dc=%.1f rf=%u", vdc, battery, dc, d[14]);
  } else if (block == 1) {
    // partition_status at d[13..20], 4 bytes/partition. Armed = stay|sleep|arm bits in byte0.
    const bool p1_armed = (d[13] & 0x07) != 0;
    const bool p2_armed = (d[17] & 0x07) != 0;
    ESP_LOGD(TAG, "RAM1: partition1 armed=%d partition2 armed=%d", p1_armed, p2_armed);
    // (Partition arm entities can be wired here if desired.)
  }
}

void ParadoxSpectra::log_unknown_(uint8_t zone, const char *label, const char *kind,
                                  const char *state) {
  const bool first = this->seen_unknown_.insert(zone).second;
  if (first)
    ESP_LOGW(TAG,
             "DISCOVERED zone %u '%s' (%s %s) — not in config; add:  - { zone: %u, "
             "name: \"%s\"%s }",
             zone, label, kind, state, zone, label,
             (std::strcmp(kind, "fire") == 0) ? ", type: fire, device_class: smoke" : "");
  else
    ESP_LOGD(TAG, "unregistered zone %u '%s' (%s %s)", zone, label, kind, state);
}

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

void ParadoxSpectra::dump_config() {
  ESP_LOGCONFIG(TAG, "Paradox Spectra decoder");
  ESP_LOGCONFIG(TAG, "  mode: %s", this->poll_interval_ > 0 ? "active (login + RAM poll)" : "passive (events only)");
  if (this->poll_interval_ > 0)
    ESP_LOGCONFIG(TAG, "  poll_interval: %u ms", this->poll_interval_);
  ESP_LOGCONFIG(TAG, "  debug_frames: %s | log_all_events: %s", YESNO(this->debug_frames_),
                YESNO(this->log_all_events_));
  ESP_LOGCONFIG(TAG, "  bound zones: open=%u fire=%u", (unsigned) this->open_.size(),
                (unsigned) this->fire_.size());
}

}  // namespace paradox_spectra
}  // namespace esphome
