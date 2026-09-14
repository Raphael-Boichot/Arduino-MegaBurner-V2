/**
 * MX26L6420
 *
 * See MX26L6420.h for the datasheet-verified details and how this
 * differs from MX29LV320E (mainly: single-cycle reset, different ID).
 */
#include "MX26L6420.h"

MX26L6420::MX26L6420()
{
}

void MX26L6420::init()
{
	  // Set Address Pins to Output
	  //A0-A7
	  DDRF = 0xFF;
	  //A8-A15
	  DDRK = 0xFF;
	  //A16-A23
	  DDRL = 0xFF;

	  // Set Control Pins to Output OE(PH1) CE(PH6), plus PH3 and PH4
	  // (both need to be outputs regardless of role - see the WE#/A21
	  // pin-swap note in MX26L6420.h: PH3 is this chip's real WE#,
	  // PH4 lands on A21 which isn't driven from here at all).
	  DDRH |=  (1 << 1) | (1 << 3) | (1 << 4) | (1 << 6);

	  // Set Data Pins (D0-D15) to Input
	  DDRC = 0x00;
	  DDRA = 0x00;
	  // Disable Internal Pullups
	  PORTC = 0x00;
	  PORTA = 0x00;

	  // Setting OE(PH1) HIGH, WE#(PH3) HIGH (idle - see MX26L6420.h),
	  // and PH4 HIGH (harmless either way, since it doesn't reach a
	  // control pin on this chip)
	  PORTH |= (1 << 1) | (1 << 3) | (1 << 4);
	  // Setting CE(PH6) LOW
	  PORTH &= ~(1 << 6);

	  delay(100);

	  // ID flash
	  readId();
	  reset();
}

void MX26L6420::readId() {
  // Set data pins to output
  dataOut();

  // Autoselect / Silicon-ID-Read command sequence (Table 4)
  writeWord(UNLOCK_ADDR_1, 0xaa);
  writeWord(UNLOCK_ADDR_2, 0x55);
  writeWord(UNLOCK_ADDR_1, 0x90);

  // Set data pins to input again
  dataIn();

  // Manufacturer ID (address 0, expect C2h) then device ID low byte
  // (address 1, expect FCh - full device code is 22FCh, but only the
  // low byte is read to keep the same 4-hex-char id format the host
  // protocol already expects, matching MX29L3211/MX29LV320E's readId()).
  // Zero-pad each byte to exactly 2 hex characters (Serial.print(b, HEX)
  // drops leading zeros, e.g. 0x0F prints as "F" and 0x00 prints as
  // nothing at all - the host always expects exactly 4 characters total
  // for the id, so a chip whose id bytes happen to be < 0x10 would
  // otherwise send fewer than 4 characters and the host would time out
  // waiting for bytes that are never coming.
  for (int i=0; i<2; i++) {
	  byte b = readByte(i);
	  if (b < 0x10) {
		  Serial.print('0');
	  }
	  Serial.print(b, HEX);
  }
}

void MX26L6420::reset() {
  // Set data pins to output
  dataOut();

  // Reset is a SINGLE bus cycle for this chip - no unlock prefix,
  // don't-care address (Table 4: "Reset | 1 cycle | XXX | F0"). This
  // is different from MX29L3211/MX29LV320E, which both need a 3-cycle
  // unlock+F0 sequence - do not "fix" this to match them.
  writeWord(0x0000, 0xf0);

  // Set data pins to input again
  dataIn();

  delay(500);
}

void MX26L6420::read16(long block_id, long block_size) {

	long start = (block_id * block_size)/2;

	for (long i = start; i < start+block_size/2; i++) {
	    word aword = readWord(i);

	    Serial.write( aword & 0xFF );
	    Serial.write( ( aword >> 8 ) & 0xFF );
	}
}

bool MX26L6420::erase() {
  // Set data pins to output
  dataOut();

  // Chip Erase command sequence (Table 4) - identical to MX29LV320E's.
  writeWord(UNLOCK_ADDR_1, 0xaa);
  writeWord(UNLOCK_ADDR_2, 0x55);
  writeWord(UNLOCK_ADDR_1, 0x80);
  writeWord(UNLOCK_ADDR_1, 0xaa);
  writeWord(UNLOCK_ADDR_2, 0x55);
  writeWord(UNLOCK_ADDR_1, 0x10);

  // Set data pins to input again
  dataIn();

  // Chip erase affects the whole array, so address 0 is always a
  // valid address to poll here (unlike write, see write16() below).
  // Erased state is all-1s (0xFFFF), so that's the expected result.
  return busyCheck(0, 0xFFFF, ERASE_TIMEOUT_MS);
}

bool MX26L6420::write16(long offset, long page_size, long block_size, byte data[]) {

	// Set data pins to output
	dataOut();

	long lastAddress = offset/2;
	word lastData = 0xFFFF; // nothing written yet (erased state)

	for (long bi = 0; bi < block_size/2; bi+=page_size/2) {
		// Check if write is complete - poll at the address actually
		// being programmed (the last word written), against the value
		// actually written there (not a fixed address/value).
		delayMicroseconds(100);
		if (!busyCheck(lastAddress, lastData, PAGE_TIMEOUT_MS)) {
			dataIn();
			return false;
		}

		// Write command sequence (Table 4) - single-word program,
		// identical structure to MX29LV320E.
		writeWord(UNLOCK_ADDR_1, 0xaa);
		writeWord(UNLOCK_ADDR_2, 0x55);
		writeWord(UNLOCK_ADDR_1, 0xa0);

		for (long pi = 0; pi < page_size/2; pi++) {
			long address = offset/2 +bi + pi;
			long a = (bi + pi)*2;
			long b = a + 1;

			word currWord = ((data[b] & 0xFF) << 8) | (data[a] & 0xFF);

			writeWord(address, currWord);
			lastAddress = address;
			lastData = currWord;
		}
	}

	// Check if write is complete
	delayMicroseconds(100);
	bool ok = busyCheck(lastAddress, lastData, PAGE_TIMEOUT_MS);
	// Set data pins to input again
	dataIn();
	return ok;
}
