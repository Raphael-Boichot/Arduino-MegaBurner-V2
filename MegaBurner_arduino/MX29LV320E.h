/**
 * MX29LV320E T/B
 *
 * 32Mbit [4M x 8 / 2M x 16] 3V flash, Macronix. Covers BOTH the
 * Top-Boot (MX29LV320ET...) and Bottom-Boot (MX29LV320EB...) variants -
 * they share the same command set; only the returned device ID differs
 * (22A7h Top / 22A8h Bottom), which the HOST side checks, not this class.
 *
 * Ported from MX29L3211.h, with two real differences confirmed against
 * the datasheet (Macronix PM1575 REV 1.3, DEC 19 2013):
 *
 *  1. Unlock addresses are 0x555 / 0x2AA here, not 0x5555 / 0x2AAA.
 *  2. This chip has NO separate "read status register" (0x70) command.
 *     Status (Data# Polling) is read via a plain read AT THE ADDRESS
 *     CURRENTLY BEING PROGRAMMED/ERASED - not a fixed address 0 - so
 *     busyCheck()/readStatusReg() take an address parameter here.
 *
 * Word-program is a standard single-word program (unlock + A0h + one
 * word), unlike the MX29L3211's own multi-word page-buffer feature -
 * this already falls out correctly from write16() as long as the host
 * sends page_size=2 (one word) for this chip, so write16() itself is
 * otherwise unchanged.
 */
#ifndef MX29LV320E_h
#define MX29LV320E_h

#include "Arduino.h"
#include "FlashChip.h"

class MX29LV320E : public FlashChip {

  public:
	char id[4];
	MX29LV320E();
	void init();
	void readId();
	void reset();
	void read8(long block_id, long block_size);
	bool write8(long offset, long page_size, long block_size, byte data[]);
	void read16(long block_id, long block_size);
	bool write16(long offset, long page_size, long block_size, byte data[]);
	bool erase();

  private:
    // Unlock addresses for this chip family (word mode). See Table 3,
    // "MX29LV320E T/B COMMAND DEFINITIONS" in the datasheet.
    static const unsigned long UNLOCK_ADDR_1 = 0x555;
    static const unsigned long UNLOCK_ADDR_2 = 0x2AA;

    // Generous timeouts so a genuinely-stuck operation (e.g. trying to
    // reprogram a cell that can't reach its target value, which can
    // never actually finish) reports failure within a bounded time
    // instead of hanging the Arduino forever, requiring a manual reset.
    // Both are far above any legitimate operation's real duration -
    // this should never fire on healthy hardware.
    static const unsigned long PAGE_TIMEOUT_MS = 1000;      // per page/word program
    static const unsigned long ERASE_TIMEOUT_MS = 120000;   // whole-chip erase (~50s max per spec)

    // Switch data pins to write
    void dataOut() {
      DDRC = 0xFF;
      DDRA = 0xFF;
    }

    // Switch data pins to read
    void dataIn() {
      DDRC = 0x00;
      DDRA = 0x00;
    }

    void writeByte(unsigned long address, byte data) {
      PORTF = address & 0xFF;
      PORTK = (address >> 8) & 0xFF;
      PORTL = (address >> 16) & 0xFF;
      PORTC = data;

      // Arduino running at 16Mhz -> one nop = 62.5ns
      // Wait till output is stable
      __asm__("nop\n\t");

      // Switch WE(PH4) to LOW
      PORTH &= ~(1 << 4);

      // Leave WE low for at least 60ns
      __asm__("nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t");

      // Switch WE(PH4) to HIGH
      PORTH |= (1 << 4);

      // Leave WE high for at least 50ns
      __asm__("nop\n\t");
    }

    void writeWord(unsigned long address, word data) {
      PORTF = address & 0xFF;
      PORTK = (address >> 8) & 0xFF;
      PORTL = (address >> 16) & 0xFF;
      PORTC = data;
      PORTA = (data >> 8) & 0xFF;

      // Arduino running at 16Mhz -> one nop = 62.5ns
      // Wait till output is stable
      __asm__("nop\n\t");

      // Switch WE(PH4) to LOW
      PORTH &= ~(1 << 4);

      // Leave WE low for at least 60ns
      __asm__("nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t");

      // Switch WE(PH4) to HIGH
      PORTH |= (1 << 4);

      // Leave WE high for at least 50ns
      __asm__("nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t");
    }

    byte readByte(unsigned long address) {
      PORTF = address & 0xFF;
      PORTK = (address >> 8) & 0xFF;
      PORTL = (address >> 16) & 0xFF;

      // Arduino running at 16Mhz -> one nop = 62.5ns
      __asm__("nop\n\t");

      // Setting OE(PH1) LOW
      PORTH &= ~(1 << 1);

      __asm__("nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t");

      // Read
      byte tempByte = PINC;

      // Setting OE(PH1) HIGH
      PORTH |= (1 << 1);
      __asm__("nop\n\t");

      return tempByte;
    }

    word readWord(unsigned long address) {
      PORTF = address & 0xFF;
      PORTK = (address >> 8) & 0xFF;
      PORTL = (address >> 16) & 0xFF;

      // Arduino running at 16Mhz -> one nop = 62.5ns
      __asm__("nop\n\t");

      // Setting OE(PH1) LOW
      PORTH &= ~(1 << 1);

      __asm__("nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t");

      // Read
      word tempWord = ( ( PINA & 0xFF ) << 8 ) | ( PINC & 0xFF );

      __asm__("nop\n\t");

      // Setting OE(PH1) HIGH
      PORTH |= (1 << 1);
      __asm__("nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t");

      return tempWord;
    }

    // No 0x70 "read status register" command for this chip - status is
    // read via a plain read at the relevant address, per the Data#
    // Polling algorithm (datasheet Figure 20). Handled inline in
    // busyCheck() below (which also needs two consecutive reads for
    // the DQ6 toggle check, so there's no separate single-read helper).

    // NOTE the address parameter: "For programming, valid address means
    // program address. For erasing, valid address means erase sectors
    // address" (datasheet, Data# Polling Algorithm notes) - unlike a
    // fixed address, this must be the address actually being acted on.
    //
    // NOTE the expectedData parameter: DQ7 settles to the COMPLEMENT of
    // the target data while busy, then the TRUE final data once done -
    // it is NOT always 1. For chip erase the expected result is all-1s
    // (0xFFFF), so checking bit 7 == 1 happens to be correct there, but
    // for program it depends on the actual data written (roughly half
    // of all words have bit 7 == 0 in their true value) - comparing
    // against a hardcoded 1 hangs forever on any such word. Comparing
    // against expectedData's own bit 7 is correct for both cases.
    //
    // DQ7 alone can look "done" a moment before the word has actually
    // fully settled, which was observed on real hardware as bit 6
    // (0x40, high byte) coming back flipped in ~47% of written words -
    // exactly the DQ6 "Toggle Bit" position. DQ6 is a second, standard,
    // independent busy indicator: it keeps toggling between successive
    // reads while busy, and stops once truly done. Requiring BOTH DQ7
    // to match the target value AND DQ6 to have stopped toggling (two
    // consecutive identical reads) before exiting is the standard,
    // robust combination and fixes that corruption.
    //
    // timeoutMs: if neither condition is ever satisfied (e.g. the
    // target data is physically impossible to reach, such as trying
    // to set a bit back to 1 without erasing first), this gives up
    // after timeoutMs and returns false instead of looping forever.
    bool busyCheck(unsigned long address, word expectedData, unsigned long timeoutMs) {
      dataIn();
      unsigned long start = millis();
      bool ok = true;
      word prevReg = readWord(address);
      while (true) {
        word curReg = readWord(address);
        bool dq7Match  = (((curReg ^ expectedData) & 0x0080) == 0);
        bool dq6Stable = (((curReg ^ prevReg) & 0x0040) == 0);
        if (dq7Match && dq6Stable) {
          break;
        }
        if (millis() - start > timeoutMs) {
          ok = false;
          break;
        }
        prevReg = curReg;
      }

      // Set data pins to output
      dataOut();
      return ok;
    }
};

#endif
