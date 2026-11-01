// tasnode hooks for upstream files. Included by every build: without -DTASNODE
// the macros compile to nothing, so upstream files need only one line per hook
// and rebasing on new ScotMesh releases stays mechanical.

#pragma once

#include <stdint.h>

#ifdef TASNODE

#define PROV_NS_TASNODE 150   // App range 100-199; see Provisioning.h

bool tasnode_accept_freq(uint32_t hz);
bool tasnode_accept_bw(uint32_t hz);
bool tasnode_accept_txp(int txp);
const char* tasnode_check_settings(uint32_t hz, uint32_t bw, int txp);  // nullptr if legal
int  tasnode_preset_txp();
void tasnode_enforce_txp();
bool tasnode_start_allowed();
void tasnode_register_provisioning();

#define TASNODE_ONLY_IF(cond) if (cond)
#define TASNODE_ENFORCE_TXP() tasnode_enforce_txp()
#define TASNODE_RADIO_GATE() do { if (!radio_locked && !tasnode_start_allowed()) radio_locked = true; } while (0)
#define TASNODE_REGISTER_PROVISIONING() tasnode_register_provisioning()

#else

#define TASNODE_ONLY_IF(cond)
#define TASNODE_ENFORCE_TXP()
#define TASNODE_RADIO_GATE()
#define TASNODE_REGISTER_PROVISIONING()

#endif
