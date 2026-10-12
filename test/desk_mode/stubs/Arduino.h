#pragma once

// Host stand-in for the Arduino clock. Time only moves when the scripted socket or delay() advances it, so
// deadline behaviour is deterministic.
#include <cstddef>
#include <cstdint>

inline uint32_t fakeClockMs = 0;
inline unsigned long millis() { return fakeClockMs; }
inline void delay(unsigned long ms) { fakeClockMs += static_cast<uint32_t>(ms); }
