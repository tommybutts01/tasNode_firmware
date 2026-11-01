// tasnode firmware glue: antenna config and the radio clamp at every point a
// setting can enter. The calculations are in radio_limits.h.
//
// Included once, from RNode_Firmware.ino, after Utilities.h.
//
// Every check here applies to radio configuration only. Nothing in this file
// sees relayed traffic, and its log lines carry no hashes.

#pragma once

#ifdef TASNODE

#include "radio_limits.h"

#ifdef HAS_PROVISIONING
#include <microReticulum/Provisioning/Provisioning.h>
#endif

// Antenna figures live in EEPROM, not on the filesystem: the radio starts in
// validate_status(), before the filesystem mounts. EEPROM also survives
// ScotMesh's full reset, which suits a fact about the hardware. These addresses
// are the top of the gap after ADDR_CONF_SMUP in ROM.h, to stay clear of
// anything upstream adds there.
#define ADDR_TASNODE_MAGIC 0xC3
#define ADDR_TASNODE_GAIN  0xC4   // int16 tenths of a dBi, big endian
#define ADDR_TASNODE_LOSS  0xC6   // int16 tenths of a dB, big endian
#define TASNODE_MAGIC_BYTE 0x54

static_assert(ADDR_TASNODE_LOSS + 1 < EEPROM_RESERVED, "tasnode config must stay in reserved EEPROM");

static bool    tasnode_loaded = false;
static int16_t tasnode_gain_tenths = tasnode::DEFAULT_GAIN_TENTHS;
static int16_t tasnode_loss_tenths = tasnode::DEFAULT_LOSS_TENTHS;
static char    tasnode_msg[56];

static int16_t tasnode_eeprom_read16(int addr) {
  return (int16_t)(((uint16_t)EEPROM.read(eeprom_addr(addr)) << 8) | EEPROM.read(eeprom_addr(addr + 1)));
}

static void tasnode_eeprom_write16(int addr, int16_t value) {
  eeprom_update(eeprom_addr(addr), (uint8_t)((uint16_t)value >> 8));
  eeprom_update(eeprom_addr(addr + 1), (uint8_t)((uint16_t)value & 0xFF));
}

// Loaded on first use, because the first caller runs during validate_status().
static void tasnode_load() {
  if (tasnode_loaded) return;
  tasnode_loaded = true;
  if (EEPROM.read(eeprom_addr(ADDR_TASNODE_MAGIC)) != TASNODE_MAGIC_BYTE) return;
  const int16_t gain = tasnode_eeprom_read16(ADDR_TASNODE_GAIN);
  const int16_t loss = tasnode_eeprom_read16(ADDR_TASNODE_LOSS);
  if (tasnode::gain_in_range(gain) && tasnode::loss_in_range(loss)) {
    tasnode_gain_tenths = gain;
    tasnode_loss_tenths = loss;
  } else {
    printf("[tasnode] stored antenna figures out of range, using defaults\n");
  }
}

static tasnode::Check tasnode_check(uint32_t hz, uint32_t bw, int txp) {
  tasnode_load();
  return tasnode::check_radio(hz, bw, txp, tasnode_gain_tenths, tasnode_loss_tenths);
}

static const char* tasnode_describe(const tasnode::Check& c, int txp) {
  tasnode::format_error(c, txp, tasnode_msg, sizeof tasnode_msg);
  printf("[tasnode] %s\n", tasnode_msg);
  return tasnode_msg;
}

const char* tasnode_check_settings(uint32_t hz, uint32_t bw, int txp) {
  const tasnode::Check c = tasnode_check(hz, bw, txp);
  return c.verdict == tasnode::Verdict::Ok ? nullptr : tasnode_describe(c, txp);
}

bool tasnode_accept_freq(uint32_t hz) {
  return tasnode_check_settings(hz, 0, tasnode::UNSET_TXP) == nullptr;
}

bool tasnode_accept_txp(int txp) {
  return tasnode_check_settings(tasnode::UNSET_HZ, lora_bw, txp) == nullptr;
}

// Hosts send frequency, then bandwidth, then TX power, so this is the first step
// that can test the whole signal against the band. Refusing a narrower bandwidth
// because of the old power would fail a legal change, so power comes down first
// instead, and the host's own power setting is checked against the new limit.
bool tasnode_accept_bw(uint32_t hz) {
  const tasnode::Check c = tasnode_check(lora_freq, hz, lora_txp);
  if (c.verdict == tasnode::Verdict::Ok) return true;
  if (c.verdict != tasnode::Verdict::TxOverLimit) { tasnode_describe(c, lora_txp); return false; }
  printf("[tasnode] TX lowered from %d to limit %d dBm for this bandwidth\n", lora_txp, c.limit_dbm);
  lora_txp = c.limit_dbm;
  if (op_mode == MODE_HOST) setTXPower();
  return true;
}

int tasnode_preset_txp() {
  tasnode_load();
  return tasnode::max_tx_dbm(tasnode::PRESET_BW_HZ, tasnode_gain_tenths, tasnode_loss_tenths);
}

// Last line of defence on the way to the radio: covers the trial revert and the
// low-battery restore, which can hold a value from before an antenna change.
void tasnode_enforce_txp() {
  const tasnode::Check c = tasnode_check(tasnode::UNSET_HZ, lora_bw, lora_txp);
  if (c.verdict != tasnode::Verdict::TxOverLimit) return;
  printf("[tasnode] TX lowered from %d to limit %d dBm\n", lora_txp, c.limit_dbm);
  lora_txp = c.limit_dbm;
}

// Called whenever the radio is about to start, so settings that arrived another
// way (a raw EEPROM write, or a config saved by older firmware) are caught here.
// Power over the limit is lowered, so the node stays legal and keeps relaying.
// A frequency out of band can't be corrected, so the radio stays off.
bool tasnode_start_allowed() {
  const tasnode::Check c = tasnode_check(lora_freq, lora_bw, lora_txp);
  if (c.verdict == tasnode::Verdict::Ok) return true;
  if (c.verdict == tasnode::Verdict::TxOverLimit) { tasnode_enforce_txp(); return true; }
  tasnode_describe(c, lora_txp);
  printf("[tasnode] radio stays off\n");
  return false;
}

#ifdef HAS_PROVISIONING
// A change that would put the current TX power over the new limit is refused,
// rather than silently lowering the power of a node that's in service.
static bool tasnode_set_antenna(int gain, int loss) {
  tasnode_load();
  if (!tasnode::gain_in_range(gain) || !tasnode::loss_in_range(loss)) return false;
  const tasnode::Check c = tasnode::check_radio(tasnode::UNSET_HZ, lora_bw, lora_txp, (int16_t)gain, (int16_t)loss);
  if (c.verdict != tasnode::Verdict::Ok) { tasnode_describe(c, lora_txp); return false; }
  eeprom_update(eeprom_addr(ADDR_TASNODE_MAGIC), 0x00);   // a partial write reads as unset
  tasnode_eeprom_write16(ADDR_TASNODE_GAIN, (int16_t)gain);
  tasnode_eeprom_write16(ADDR_TASNODE_LOSS, (int16_t)loss);
  eeprom_update(eeprom_addr(ADDR_TASNODE_MAGIC), TASNODE_MAGIC_BYTE);
  tasnode_gain_tenths = (int16_t)gain;
  tasnode_loss_tenths = (int16_t)loss;
  return true;
}

void tasnode_register_provisioning() {
  using namespace RNS::Provisioning;
  tasnode_load();
  Provisioner::instance()
    .register_namespace("tasnode Antenna", PROV_NS_TASNODE)
      .field_float("Antenna Gain (dBi)", 1, FF_LIVE_APPLY,
        tasnode_gain_tenths / 10.0, tasnode::MIN_GAIN_TENTHS / 10.0, tasnode::MAX_GAIN_TENTHS / 10.0,
        [](const Value& v) { return tasnode_set_antenna((int)lround(v.as_float() * 10.0), tasnode_loss_tenths); },
        []() { return tasnode_gain_tenths / 10.0; })
      .field_float("Feeder Loss (dB)", 2, FF_LIVE_APPLY,
        tasnode_loss_tenths / 10.0, tasnode::MIN_LOSS_TENTHS / 10.0, tasnode::MAX_LOSS_TENTHS / 10.0,
        [](const Value& v) { return tasnode_set_antenna(tasnode_gain_tenths, (int)lround(v.as_float() * 10.0)); },
        []() { return tasnode_loss_tenths / 10.0; })
      .end();
}
#else
void tasnode_register_provisioning() {}
#endif

#endif // TASNODE
