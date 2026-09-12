#pragma once

#include <Mesh.h>
#include <RadioLib.h>
#include "../RxDutyCycleCalc.h"

#ifdef USE_CC310_HW_CRYPTO
#include <Adafruit_nRFCrypto.h>
#endif
struct PacketMillis {
  uint32_t preambleMillis;  // preamble-detect -> header-valid deadline
  uint32_t payloadMillis;   // header-valid   -> rx-done deadline
};

class RadioLibWrapper : public mesh::Radio {
protected:
  PhysicalLayer* _radio;
  mesh::MainBoard* _board;
  uint32_t n_recv, n_sent, n_recv_errors;
  int16_t _noise_floor, _threshold;
  bool _cad_enabled;
  uint16_t _num_floor_samples;
  int32_t _floor_sample_sum;
  uint8_t _preamble_sf;
  bool _duty_cycle_wanted;         // requested via prefs/CLI; actual use also needs _duty_window.supported
  RxDutyCycleWindow _duty_window;  // current Rx/sleep window, recomputed on every setParams()

  void idle();
  void startRecv();
  bool doStartReceive();   // shared by startRecv() and recvRaw()'s re-arm path
  float packetScoreInt(float snr, int sf, int packet_len);
  virtual bool isReceivingPacket() =0;
  virtual void doResetAGC();

  // Chip-autonomous RX duty cycling ("sniff mode"). Only the SX126x family can do this in
  // hardware; overridden by SX126xDutyCycleWrapper. See src/helpers/RxDutyCycleCalc.h.
  virtual RadioFamily getRadioFamily() const { return RadioFamily::UNSUPPORTED; }
  virtual bool startReceiveDutyCycleRaw(uint32_t rxPeriodUs, uint32_t sleepPeriodUs) { return false; }

public:
  RadioLibWrapper(PhysicalLayer& radio, mesh::MainBoard& board)
    : _radio(&radio), _board(&board), _preamble_sf(0), _duty_cycle_wanted(false), _duty_window{false, 0, 0}
  { n_recv = n_sent = 0; }

  void begin() override;
  virtual void powerOff() { _radio->sleep(); }
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isChannelActive();

  bool isReceiving() override {
    if (isReceivingPacket()) return true;

    return isChannelActive();
  }

  virtual void setParams(float freq, float bw, uint8_t sf, uint8_t cr) = 0;
  uint32_t getRngSeed();
  void setTxPower(int8_t dbm);

  virtual float getCurrentRSSI() =0;
  virtual uint8_t getSpreadingFactor() const { return LORA_SF; }
  static uint16_t preambleLengthForSF(uint8_t sf) { return sf <= 8 ? 32 : 16; }
  void updatePreamble(uint8_t sf) { _preamble_sf = sf; _radio->setPreambleLength(preambleLengthForSF(sf)); }
  void updateDutyCycleWindow(uint8_t sf, float bw, uint16_t preambleSymbols) {
    _duty_window = calcRxDutyCycleWindow(getRadioFamily(), sf, bw, preambleSymbols);
  }
  PacketMillis calcMaxPacketMillis(uint8_t sf, float bw, uint8_t cr, uint8_t preambleSymbols);
  virtual int16_t performChannelScan();

  int getNoiseFloor() const override { return _noise_floor; }
  void triggerNoiseFloorCalibrate(int threshold) override;
  void setCADEnabled(bool enable) override { _cad_enabled = enable; }
  void resetAGC() override;

  void loop() override;

  uint32_t getPacketsRecv() const { return n_recv; }
  uint32_t getPacketsRecvErrors() const { return n_recv_errors; }
  uint32_t getPacketsSent() const { return n_sent; }
  void resetStats() { n_recv = n_sent = n_recv_errors = 0; }

  virtual float getLastRSSI() const override;
  virtual float getLastSNR() const override;

  float packetScore(float snr, int packet_len) override { return packetScoreInt(snr, 10, packet_len); }  // assume sf=10

  virtual bool setRxBoostedGainMode(bool) { return false; }
  virtual bool getRxBoostedGainMode() const { return false; }
  
  virtual bool configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) { return false; }

  // Chip-autonomous RX duty cycling ("sniff mode"), default OFF. Enabling is a request only:
  // isRxDutyCycleEnabled() also requires the current SF/BW/preamble combination to have a safe
  // sleep window (see RxDutyCycleCalc.h) -- otherwise continuous RX is used regardless.
  bool supportsRxDutyCycle() const { return getRadioFamily() == RadioFamily::SX126X; }
  void setRxDutyCycleEnabled(bool en) { _duty_cycle_wanted = en; }
  bool isRxDutyCycleEnabled() const { return _duty_cycle_wanted && _duty_window.supported; }
};

/**
 * \brief  Shared support for chip-autonomous RX duty cycling ("sniff mode"), available on every
 *         RadioLib radio derived from SX126x (SX1262, SX1268, LLCC68, STM32WLx all share the same
 *         SetRxDutyCycle command). NOT available on LR1110 (known mid-preamble lock erratum) or
 *         SX127x (no equivalent radio command) -- those keep the base class's unsupported path.
 */
class SX126xDutyCycleWrapper : public RadioLibWrapper {
protected:
  SX126xDutyCycleWrapper(SX126x& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

  RadioFamily getRadioFamily() const override { return RadioFamily::SX126X; }

  bool startReceiveDutyCycleRaw(uint32_t rxPeriodUs, uint32_t sleepPeriodUs) override {
    // Must include PREAMBLE_DETECTED in the IRQ flags, matching the Custom*::startReceive()
    // override used for continuous RX -- otherwise isReceivingPacket() (used by Dispatcher's CAD
    // check before TX) can never observe a mid-cycle preamble detection, silently breaking
    // collision avoidance while duty cycling is active.
    return ((SX126x*)_radio)->startReceiveDutyCycle(rxPeriodUs, sleepPeriodUs,
        RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED), RADIOLIB_IRQ_RX_DEFAULT_MASK) == RADIOLIB_ERR_NONE;
  }
};

/**
 * \brief  an RNG impl using the noise from the LoRa radio as entropy.
 *         NOTE: this is VERY SLOW!  Use only for things like creating new LocalIdentity
*/
class RadioNoiseListener : public mesh::RNG {
  PhysicalLayer* _radio;
public:
  RadioNoiseListener(PhysicalLayer& radio): _radio(&radio) { }

  void random(uint8_t* dest, size_t sz) override {
#ifdef USE_CC310_HW_CRYPTO
    nRFCrypto.Random.generate(dest, (uint16_t)sz);
    for (int i = 0; i < sz; i++) {
      dest[i] ^= _radio->randomByte() ^ (::random(0, 256) & 0xFF); // combine with Radio's entropy
    }
#else
    for (int i = 0; i < sz; i++) {
      dest[i] = _radio->randomByte() ^ (::random(0, 256) & 0xFF);
    }
#endif
  }
};
