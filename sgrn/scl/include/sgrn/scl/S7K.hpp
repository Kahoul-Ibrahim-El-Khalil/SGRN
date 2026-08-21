#pragma once
// ── S7 protocol constants (self-contained, no snap7.h required) ─────────────
// When building inside the SGRN monorepo with snap7 available, the build system
// defines SGRN_HAS_SNAP7 and snap7.h provides these symbols.
// When building standalone (without snap7), we define them ourselves.
#ifdef SGRN_HAS_SNAP7
#include <snap7.h>
#else
using byte = unsigned char;
inline constexpr byte S7AreaPE = 0x81; // Process inputs  (I/E)
inline constexpr byte S7AreaPA = 0x82; // Process outputs (Q/A)
inline constexpr byte S7AreaMK = 0x83; // Merkers         (M)
inline constexpr byte S7AreaDB = 0x84; // Data blocks     (DB)
inline constexpr byte S7AreaCT = 0x1C; // Counters        (C/Z)
inline constexpr byte S7AreaTM = 0x1D; // Timers          (T)

inline constexpr int S7WLBit = 0x01;
inline constexpr int S7WLByte = 0x02;
inline constexpr int S7WLWord = 0x04;
inline constexpr int S7WLDWord = 0x06;
inline constexpr int S7WLReal = 0x08;
inline constexpr int S7WLCounter = 0x1C;
inline constexpr int S7WLTimer = 0x1D;
#endif
