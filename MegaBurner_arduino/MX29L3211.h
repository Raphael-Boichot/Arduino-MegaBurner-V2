/**
 * MX29L3211
 *
 * Version 1.0
 * Date 2017.11.11
 * Contact: mingzo@gmail.com
 */
#ifndef MX29L3211_h
#define MX29L3211_h

#include "Arduino.h"
#include "FlashChip.h"

class MX29L3211 : public FlashChip {

  public:
	char id[4];
	MX29L3211();
	void init();
	void readId();
	void reset();
	void read8(long block_id, long block_size);
	bool write8(long offset, long page_size, long block_size, byte data[]);
	void read16(long block_id, long block_size);
	bool write16(long offset, long page_size, long block_size, byte data[]);
	bool erase();

  private:
    // Generous timeouts so a genuinely-stuck operation (e.g. trying to
    // reprogram a cell that can't reach its target value, which can
    // never actually finish) reports failure within a bounded time
    // instead of hanging the Arduino forever, requiring a manual reset.
    // Both are far above any legitimate operation's real duration -
    // this should never fire on healthy hardware.
    static const unsigned long PAGE_TIMEOUT_MS = 1000;      // per page/word program
    static const unsigned long ERASE_TIMEOUT_MS = 600000;    // whole-chip erase (generous margin above ~90s spec)

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

    word readStatusReg() {
      // Set data pins to output
      dataOut();

      // Status reg command sequence
      writeWord(0x5555, 0xaa);
      writeWord(0x2aaa, 0x55);
      writeWord(0x5555, 0x70);

      // Set data pins to input again
      dataIn();

      // Read the status register
      return readWord(0);
    }

    bool busyCheck(unsigned long timeoutMs) {
      // Read the status register
      word statusReg = readStatusReg();
      unsigned long start = millis();
      bool ok = true;

      while ((statusReg | 0xFF7F) != 0xFFFF) {
        if (millis() - start > timeoutMs) {
          ok = false;
          break;
        }
        statusReg = readWord(0);
      }

      // Set data pins to output
      dataOut();
      return ok;
    }
};

#endif

