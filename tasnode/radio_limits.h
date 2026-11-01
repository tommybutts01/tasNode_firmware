// tasnode radio limits: pure calculations, no Arduino dependencies, so the
// native unit tests in test/ exercise exactly what the firmware runs.
//
// Implements section 7.2 of the TASNET firmware spec against the LIPD Class
// Licence 2025 (Schedule 1, Table 8, item 6 and clause 43). Check the current
// compilation of the licence before fielding anything; this is not legal
// advice. Every regulatory number lives in this file.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace tasnode {

constexpr uint32_t BAND_LOW_HZ  = 915000000;  // exclusive
constexpr uint32_t BAND_HIGH_HZ = 928000000;  // inclusive

constexpr double TOTAL_EIRP_DBM  = 30.0;      // 1 W
constexpr double PSD_MW_PER_3KHZ = 25.0;

constexpr int RADIO_MAX_TX_DBM = 22;          // SX1262 conducted
constexpr int RADIO_MIN_TX_DBM = -9;

// Antenna figures are held in tenths of a dB, so that gain minus loss is exact.
constexpr int16_t DEFAULT_GAIN_TENTHS = 30;   // 3 dBi
constexpr int16_t DEFAULT_LOSS_TENTHS = 0;    // 0 dB
constexpr int16_t MIN_GAIN_TENTHS = -100;
constexpr int16_t MAX_GAIN_TENTHS = 300;
constexpr int16_t MIN_LOSS_TENTHS = 0;
constexpr int16_t MAX_LOSS_TENTHS = 200;

// "Tasmania" radio preset. PLACEHOLDER FREQUENCY: an in-band value for bench use
// only, not a chosen channel. The real one comes from the SDR survey (spec 7.3).
constexpr uint32_t PRESET_FREQ_HZ = 917000000;
constexpr uint32_t PRESET_BW_HZ   = 125000;
constexpr int      PRESET_SF      = 9;
constexpr int      PRESET_CR      = 5;        // 4/5

// Sentinels for a setting that hasn't been configured yet (Config.h defaults).
constexpr uint32_t UNSET_HZ  = 0;
constexpr int      UNSET_TXP = 0xFF;

// Tests the centre frequency alone. Hosts send frequency before bandwidth, so
// this is all that can be known at that step; signal_in_band() is the real test.
inline bool freq_allowed(uint32_t hz) {
  return hz > BAND_LOW_HZ && hz <= BAND_HIGH_HZ;
}

// The whole signal must fit the band, half the bandwidth either side of centre.
// Worked in doubled integers, so an exact edge isn't decided by rounding.
inline bool signal_in_band(uint32_t hz, uint32_t bw_hz) {
  const int64_t centre = 2 * static_cast<int64_t>(hz);
  const int64_t bw = static_cast<int64_t>(bw_hz);
  return centre - bw > 2 * static_cast<int64_t>(BAND_LOW_HZ)
      && centre + bw <= 2 * static_cast<int64_t>(BAND_HIGH_HZ);
}

// 25 mW x (bw / 3 kHz), in dBm. This takes clause 43's density limit as averaged
// across the bandwidth, by project decision: see D6 in tasnode-f0-findings.md.
// Callers must pass bw_hz > 0.
inline double psd_limit_dbm(uint32_t bw_hz) {
  return 10.0 * std::log10(PSD_MW_PER_3KHZ * (static_cast<double>(bw_hz) / 3000.0));
}

inline double max_eirp_dbm(uint32_t bw_hz) {
  const double psd = psd_limit_dbm(bw_hz);
  return psd < TOTAL_EIRP_DBM ? psd : TOTAL_EIRP_DBM;
}

// Highest whole-dBm setting within the limit. The radio takes integer power,
// so this rounds down, never up. Callers must pass bw_hz > 0.
inline int max_tx_dbm(uint32_t bw_hz, int16_t gain_tenths, int16_t loss_tenths) {
  const int net_tenths = static_cast<int>(gain_tenths) - static_cast<int>(loss_tenths);
  const double limit = max_eirp_dbm(bw_hz) - net_tenths / 10.0;
  const double capped = limit < RADIO_MAX_TX_DBM ? limit : RADIO_MAX_TX_DBM;
  return static_cast<int>(std::floor(capped));
}

inline bool gain_in_range(int gain_tenths) {
  return gain_tenths >= MIN_GAIN_TENTHS && gain_tenths <= MAX_GAIN_TENTHS;
}

inline bool loss_in_range(int loss_tenths) {
  return loss_tenths >= MIN_LOSS_TENTHS && loss_tenths <= MAX_LOSS_TENTHS;
}

enum class Verdict : uint8_t {
  Ok,
  FreqOutOfBand,
  TxOverLimit,
  NoLegalPower,   // antenna gain leaves no setting the radio can reach
};

struct Check {
  Verdict verdict;
  int limit_dbm;  // meaningful for TxOverLimit and NoLegalPower
};

// Checks whatever has been set. Unset values are skipped, because hosts send
// radio settings one at a time; update_radio_lock() keeps the radio off until
// all of them exist, and the gate at radio start checks the full set.
inline Check check_radio(uint32_t freq_hz, uint32_t bw_hz, int txp_dbm,
                         int16_t gain_tenths, int16_t loss_tenths) {
  if (freq_hz != UNSET_HZ && !freq_allowed(freq_hz)) return {Verdict::FreqOutOfBand, 0};
  if (bw_hz == 0) return {Verdict::Ok, 0};
  if (freq_hz != UNSET_HZ && !signal_in_band(freq_hz, bw_hz)) return {Verdict::FreqOutOfBand, 0};
  const int limit = max_tx_dbm(bw_hz, gain_tenths, loss_tenths);
  if (limit < RADIO_MIN_TX_DBM) return {Verdict::NoLegalPower, limit};
  if (txp_dbm != UNSET_TXP && txp_dbm > limit) return {Verdict::TxOverLimit, limit};
  return {Verdict::Ok, limit};
}

// Short enough for a Micron page, which has to fit in one LoRa packet.
inline void format_error(const Check& c, int txp_dbm, char* out, size_t len) {
  switch (c.verdict) {
    case Verdict::FreqOutOfBand:
      snprintf(out, len, "Rejected: signal must fit 915.000-928.000 MHz");
      break;
    case Verdict::TxOverLimit:
      snprintf(out, len, "Rejected: %d dBm, limit %d dBm here", txp_dbm, c.limit_dbm);
      break;
    case Verdict::NoLegalPower:
      snprintf(out, len, "Rejected: limit %d dBm, below radio minimum", c.limit_dbm);
      break;
    case Verdict::Ok:
      if (len > 0) out[0] = '\0';
      break;
  }
}

}  // namespace tasnode
