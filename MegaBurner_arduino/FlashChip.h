/**
 * FlashChip
 *
 * Common interface for all chip drivers (MX29L3211, MX29LV320E,
 * MX26L6420, ...), so MegaBurner_arduino.ino can select which chip is
 * physically connected at RUNTIME (via a host command) instead of
 * needing a firmware reflash per chip.
 *
 * NOTE: read8()/write8() are intentionally NOT part of this interface.
 * They're dead code in every existing chip driver (MegaBurner_arduino.ino
 * only ever calls read16()/write16() - the host always operates in
 * 16-bit/word mode), so there's no reason to force every chip driver to
 * implement/expose them polymorphically.
 */
#ifndef FlashChip_h
#define FlashChip_h

#include "Arduino.h"

class FlashChip {
  public:
	virtual void init() = 0;
	virtual void readId() = 0;
	virtual void reset() = 0;
	virtual void read16(long block_id, long block_size) = 0;
	// write16()/erase() return false if the chip never reported
	// "operation complete" within a sane timeout (instead of hanging
	// the Arduino forever) - see busyCheck() in each chip driver.
	virtual bool write16(long offset, long page_size, long block_size, byte data[]) = 0;
	virtual bool erase() = 0;

	// Base classes with virtual functions need a virtual destructor,
	// even though these chip objects are never actually deleted (they're
	// all static/global instances) - this is a C++ correctness rule, not
	// something that changes behavior here.
	virtual ~FlashChip() {}
};

#endif
