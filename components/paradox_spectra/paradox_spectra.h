#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace esphome {
namespace paradox_spectra {

// Paradox Spectra SP-series decoder for ESPHome.
// - Passive: decodes the 37-byte LiveEvent frames the panel streams on change.
// - Optional active poll (set pc_password + poll_interval, and wire UART TX): logs in
//   and reads panel RAM for current zone state + supply voltages + partition arm.
// READ-ONLY BY CAPABILITY: only login + ReadEEPROM are ever sent — no arm/disarm/PGM
// opcodes exist in this code, so it can never actuate the panel. See the README.
class ParadoxSpectra : public Component, public uart::UARTDevice {
 public:
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_debug_frames(bool v) { this->debug_frames_ = v; }
  void set_log_all_events(bool v) { this->log_all_events_ = v; }
  void set_pc_password(const std::string &v) { this->pc_password_ = v; }
  void set_poll_interval(uint32_t ms) { this->poll_interval_ = ms; }

  void register_zone_open(uint8_t zone, binary_sensor::BinarySensor *s) { this->open_[zone] = s; }
  void register_zone_fire(uint8_t zone, binary_sensor::BinarySensor *s) { this->fire_[zone] = s; }
  void register_zone_tamper(uint8_t zone, binary_sensor::BinarySensor *s) { this->tamper_[zone] = s; }
  void set_last_event(text_sensor::TextSensor *s) { this->last_event_ = s; }
  void set_supply_voltage(sensor::Sensor *s) { this->supply_voltage_ = s; }
  void set_battery_voltage(sensor::Sensor *s) { this->battery_voltage_ = s; }
  void set_dc_voltage(sensor::Sensor *s) { this->dc_voltage_ = s; }
  void set_rf_noise(sensor::Sensor *s) { this->rf_noise_ = s; }

 protected:
  static bool checksum_ok_(const uint8_t *frame);
  static const char *event_name_(uint8_t major);
  void handle_frame_(const uint8_t *frame);
  void log_unknown_(uint8_t zone, const char *label, const char *kind, const char *state);

  // Active poll (only present when poll_interval_ > 0).
  void send_frame_(uint8_t *f);            // fills byte[36]=checksum, writes 37 bytes
  void send_login_();                      // InitializeCommunication (0x00) + pc_password
  void send_read_status_(uint8_t block);   // ReadEEPROM (0x50) @ 0x8000+block
  void handle_status_(const uint8_t *data, uint8_t block);  // RAM block -> entities
  void publish_zone_bitmap_(const uint8_t *b4);             // block0 zone_open -> open_

  std::vector<uint8_t> buf_;
  std::map<uint8_t, binary_sensor::BinarySensor *> open_;
  std::map<uint8_t, binary_sensor::BinarySensor *> fire_;
  std::map<uint8_t, binary_sensor::BinarySensor *> tamper_;
  std::set<uint8_t> seen_unknown_;
  text_sensor::TextSensor *last_event_{nullptr};
  sensor::Sensor *supply_voltage_{nullptr};   // VDC (~16.7 V)
  sensor::Sensor *battery_voltage_{nullptr};
  sensor::Sensor *dc_voltage_{nullptr};
  sensor::Sensor *rf_noise_{nullptr};

  bool debug_frames_{false};
  bool log_all_events_{false};

  // Active-poll state
  std::string pc_password_{"0000"};
  uint32_t poll_interval_{0};               // 0 = passive only
  bool have_settings_{false};               // captured product_id/fw/panel_id from panel 0x00
  bool logged_in_{false};
  uint8_t product_id_{0};
  uint8_t fw_[3]{0, 0, 0};
  uint16_t panel_id_{0};
  uint8_t poll_block_{0};                   // cycles 0,1 (then back)
  uint32_t last_poll_{0};
  uint32_t last_login_try_{0};
};

}  // namespace paradox_spectra
}  // namespace esphome
