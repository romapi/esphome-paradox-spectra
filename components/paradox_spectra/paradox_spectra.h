#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <map>
#include <set>
#include <vector>

namespace esphome {
namespace paradox_spectra {

// Paradox Spectra SP-series passive decoder.
// Reads the panel serial/PRG header (9600 8N1 TTL) and decodes the 37-byte LiveEvent
// frames the panel streams unsolicited. Read-only: never transmits, so it is safe to
// tap a bus another client already drives. See the README for protocol notes.
class ParadoxSpectra : public Component, public uart::UARTDevice {
 public:
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_debug_frames(bool v) { this->debug_frames_ = v; }
  // Log EVERY decoded LiveEvent (time + event name + zone/user + label) at INFO, not
  // just the handful wired to sensors — see the panel's full chatter.
  void set_log_all_events(bool v) { this->log_all_events_ = v; }
  // zone -> sensor bindings (populated by the binary_sensor platform)
  void register_zone_open(uint8_t zone, binary_sensor::BinarySensor *s) { this->open_[zone] = s; }
  void register_zone_fire(uint8_t zone, binary_sensor::BinarySensor *s) { this->fire_[zone] = s; }
  void set_last_event(text_sensor::TextSensor *s) { this->last_event_ = s; }

 protected:
  static bool checksum_ok_(const uint8_t *frame);
  static const char *event_name_(uint8_t major);
  void handle_frame_(const uint8_t *frame);
  // Announce a zone event whose number isn't in the config — WARN on first sight
  // (with its label), DEBUG after, so you can discover zone numbers from the logs.
  void log_unknown_(uint8_t zone, const char *label, const char *kind, const char *state);

  std::vector<uint8_t> buf_;
  std::map<uint8_t, binary_sensor::BinarySensor *> open_;
  std::map<uint8_t, binary_sensor::BinarySensor *> fire_;
  std::set<uint8_t> seen_unknown_;
  text_sensor::TextSensor *last_event_{nullptr};
  bool debug_frames_{true};
  bool log_all_events_{false};
};

}  // namespace paradox_spectra
}  // namespace esphome
