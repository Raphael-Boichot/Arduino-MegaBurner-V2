/**
 * MX26L6420
 *
 * 64Mbit [4M x 16] MTP EPROM, Macronix. 16-bit ONLY - there is no
 * BYTE# pin on this chip at all (confirmed from the pin description:
 * Q0-Q15 are always active, no byte-mode fallback), unlike
 * MX29LV320E's switchable x8/x16.
 *
 * IMPORTANT: this chip is rated for only ~100 erase/program cycles
 * (vs. 100,000+ for the flash chips) - far less margin for mistakes.
 *
 * Ported from MX29LV320E.h. Verified against the datasheet
 * (Macronix P/N: PM0823, REV. 0.5, JAN. 29, 2002), Table 4 "MX26L6420
 * COMMAND DEFINITIONS":
 *
 *  - Unlock addresses: 0x555 / 0x2AA - SAME as MX29LV320E.
 *  - Chip Erase: 555/AA, 2AA/55, 555/80, 555/AA, 2AA/55, 555/10 -
 *    IDENTICAL sequence to MX29LV320E.
 *  - Program: 555/AA, 2AA/55, 555/A0, PA/PD - single-word program
 *    only (no page-buffer feature), same as MX29LV320E. Requires
 *    page_size=2 from the host, same reasoning as MX29LV320E.
 *  - Status: "the system can determine the status of the erase
 *    operation by using Q7, Q6" - same dual DQ7+DQ6 polling as
 *    MX29LV320E, for the same reasons (see busyCheck() below and the
 *    comment history in MX29LV320E.h for why DQ7 alone isn't enough).
 *  - Reset is DIFFERENT: table shows "Reset | 1 cycle | XXX | F0" -
 *    a SINGLE bus cycle, don't-care address, no unlock prefix. This
 *    is unlike MX29L3211/MX29LV320E, which both need a 3-cycle
 *    unlock+F0 sequence.
 *  - Silicon ID: manufacturer C2h at address 0, device code 22FCh at
 *    address 1 (only the low byte, FCh, is read here - see readId()
 *    in the .ino file for why that's sufficient).
 *
 * This chip also has a dedicated hardware RESET pin (active low) -
 * but it's only broken out on the 48-TSOP package, not on 44-SOP
 * (confirmed from the pinout). Since this project uses the 44-SOP
 * package, that pin simply isn't accessible here and there's nothing
 * to wire up or worry about - the chip is controlled entirely via the
 * software reset command (single F0 write, see reset() below), same
 * mechanism as the other two chips.
 *
 * IMPORTANT - WE#/A21 pin swap (software-compensated, see below):
 * This project's 44-SOP socket/adapter was wired for MX29LV320E's
 * pinout, which MX26L6420 does NOT fully share despite being the same
 * package. Comparing both chips' 44-SOP pinouts pin-by-pin, every
 * signal matches EXCEPT two:
 *
 *     Pin  | MX29LV320E | MX26L6420
 *     -----|------------|----------
 *     1    | WE#        | A21
 *     33   | BYTE#      | WE#
 *
 * MX29LV320E doesn't need an A21 pin (it's a smaller, 4MB chip) and
 * doesn't need WE# at pin 33 either (that slot is BYTE# there instead)
 * - so on MX26L6420, Macronix reused pin 1's position for the extra
 * address bit this larger chip needs, and moved WE# to where BYTE#
 * used to be.
 *
 * Consequence: on this adapter, the Arduino's normal WE#-drive pin
 * (PH4) reaches socket pin 1, which is MX26L6420's A21 - not WE# at
 * all. Meanwhile socket pin 33 - this chip's REAL WE# - receives
 * whatever the Arduino drives for "BYTE#" (PH3), which every other
 * chip driver holds permanently HIGH and never toggles. A WE# line
 * that never pulses means the chip can never latch a write command -
 * this was the actual root cause of check()/erase() getting no
 * response at all, even though plain reads worked fine (reads don't
 * involve WE# at all).
 *
 * Rather than requiring a hardware rewire, this class compensates in
 * software: writeWord() below pulses PH3 (physically wired to socket
 * pin 33, this chip's real WE#) instead of PH4. PH4 is left alone -
 * it reaches socket pin 1 (A21), which is separately tied to GND
 * outside the Arduino's control anyway (see the A21 discussion
 * elsewhere in this project - the chip is limited to its lower 4MB
 * bank as a result, which is a known, accepted limitation, not a bug).
 */
#ifndef MX26L6420_h
#define MX26L6420_h

#include "Arduino.h"
#include "FlashChip.h"

class MX26L6420 : public FlashChip {

  public:
	char id[4];
	MX26L6420();
	void init();
	void readId();
	void reset();
	void read16(long block_id, long block_size);
	void write16(long offset, long page_size, long block_size, byte data[]);
	void erase();

  private:
    static const unsigned long UNLOCK_ADDR_1 = 0x555;
    static const unsigned long UNLOCK_ADDR_2 = 0x2AA;

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

    void writeWord(unsigned long address, word data) {
      PORTF = address & 0xFF;
      PORTK = (address >> 8) & 0xFF;
      PORTL = (address >> 16) & 0xFF;
      PORTC = data;
      PORTA = (data >> 8) & 0xFF;

      // Arduino running at 16Mhz -> one nop = 62.5ns
      // Wait till output is stable
      __asm__("nop\n\t");

      // Pulse WE# LOW - PH3, NOT PH4! See the WE#/A21 pin-swap note
      // in the class comment above: PH3 is physically wired to socket
      // pin 33, which is this chip's real WE#. PH4 (the "normal" WE#
      // pin used by the other two chip drivers) reaches socket pin 1,
      // which on THIS chip is A21, not WE# - toggling PH4 here would
      // do nothing useful.
      PORTH &= ~(1 << 3);

      // Leave WE low for at least 60ns
      __asm__("nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t");

      // Switch WE# (PH3) back to HIGH
      PORTH |= (1 << 3);

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

    // See MX29LV320E.h for the full reasoning: DQ7 alone can look
    // "done" a moment before the word has actually fully settled (bit
    // 6, the DQ6 "Toggle Bit", may still be toggling) - requiring both
    // to agree is the standard, robust combination.
    void busyCheck(unsigned long address, word expectedData) {
      dataIn();
      word prevReg = readWord(address);
      while (true) {
        word curReg = readWord(address);
        bool dq7Match  = (((curReg ^ expectedData) & 0x0080) == 0);
        bool dq6Stable = (((curReg ^ prevReg) & 0x0040) == 0);
        if (dq7Match && dq6Stable) {
          break;
        }
        prevReg = curReg;
      }
      dataOut();
    }
};

#endif
