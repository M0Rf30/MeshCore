#pragma once

#include <cmath>
#include <cstdint>

namespace netsim {

// Standard Semtech LoRa time-on-air formula (AN1200.13), the same formula
// RadioLibWrapper::getEstAirtimeFor() delegates to via RadioLib's
// PhysicalLayer::getTimeOnAir() for the real hardware radios. SimRadio uses this
// directly (rather than linking RadioLib) so airtime numbers come from the same
// physics, parameterised by the project's real LoRa settings.
struct LoRaParams {
  float bandwidth_khz = 62.5f;   // matches platformio.ini LORA_BW
  uint8_t spreading_factor = 8;  // matches platformio.ini LORA_SF
  uint8_t coding_rate = 5;       // 4/5
  uint8_t preamble_symbols = 8;
  bool low_data_rate_optimize = false;
  bool explicit_header = true;
  bool crc_enabled = true;
};

inline uint32_t loRaTimeOnAirMillis(int len_bytes, const LoRaParams& p) {
  double bw_hz = (double)p.bandwidth_khz * 1000.0;
  double sf = (double)p.spreading_factor;
  double t_sym_ms = (std::pow(2.0, sf) / bw_hz) * 1000.0;

  double de = p.low_data_rate_optimize ? 1.0 : 0.0;
  double ih = p.explicit_header ? 0.0 : 1.0;
  double crc = p.crc_enabled ? 1.0 : 0.0;

  double num = 8.0 * len_bytes - 4.0 * sf + 28.0 + 16.0 * crc - 20.0 * ih;
  double denom = 4.0 * (sf - 2.0 * de);
  double n_payload_symbols = 8.0 + std::max(std::ceil(num / denom) * (p.coding_rate + 4), 0.0);

  double t_preamble_ms = (p.preamble_symbols + 4.25) * t_sym_ms;
  double t_payload_ms = n_payload_symbols * t_sym_ms;

  return (uint32_t)std::llround(t_preamble_ms + t_payload_ms);
}

}
