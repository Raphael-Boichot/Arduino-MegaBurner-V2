/**
 * MX29LV320E T/B
 *
 * See MX29LV320E.h for the differences from MX29L3211 and why they
 * were made (unlock addresses, address-aware busy check).
 */
#include "MX29LV320E.h"

MX29LV320E::MX29LV320E()
{
}

void MX29LV320E::init()
{
	  // Set Address Pins to Output
	  //A0-A7
	  DDRF = 0xFF;
	  //A8-A15
	  DDRK = 0xFF;
	  //A16-A23
	  DDRL = 0xFF;

	  // Set Control Pins to Output OE(PH1) BYTE(PH3) WE(PH4) CE(PH6)
	  DDRH |=  (1 << 1) | (1 << 3) | (1 << 4) | (1 << 6);

	  // Set Data Pins (D0-D15) to Input
	  DDRC = 0x00;
	  DDRA = 0x00;
	  // Disable Internal Pullups
	  PORTC = 0x00;
	  PORTA = 0x00;

	  // Setting OE(PH1) BYTE(PH3) WE(PH4) HIGH
	  PORTH |= (1 << 1) | (1 << 3) | (1 << 4);
	  // Setting CE(PH6) LOW
	  PORTH &= ~(1 << 6);

	  delay(100);

	  // ID flash
	  readId();
	  reset();
}

void MX29LV320E::readId() {
  // Set data pins to output
  dataOut();

  // Automatic Select command sequence (Table 3)
  writeWord(UNLOCK_ADDR_1, 0xaa);
  writeWord(UNLOCK_ADDR_2, 0x55);
  writeWord(UNLOCK_ADDR_1, 0x90);

  // Set data pins to input again
  dataIn();

  // Manufacturer ID (address 0) then device ID low byte (address 1).
  // Device ID is a full word (22A7h Top / 22A8h Bottom) but only the
  // low byte is read here to keep the same 4-hex-char id format the
  // host protocol already expects (matches MX29L3211.ino's readId()).
  // The low byte alone (A7h/A8h) is already enough to tell the two
  // variants apart.
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

void MX29LV320E::reset() {
  // Set data pins to output
  dataOut();

  // Reset command sequence
  writeWord(UNLOCK_ADDR_1, 0xaa);
  writeWord(UNLOCK_ADDR_2, 0x55);
  writeWord(UNLOCK_ADDR_1, 0xf0);

  // Set data pins to input again
  dataIn();

  delay(500);
}

void MX29LV320E::read8(long block_id, long block_size) {

	long start = (block_id * block_size)/2;

	//less than 2MB, use 8bit mode
	for (long i = start; i < (start+block_size); i++) {
		byte b = readByte(i);
		Serial.write(b);
	}

}

void MX29LV320E::read16(long block_id, long block_size) {

	long start = (block_id * block_size)/2;

	//larger than 2MB, use 16bit mode
	for (long i = start; i < start+block_size/2; i++) {
	    word aword = readWord(i);

	    Serial.write( aword & 0xFF );
	    Serial.write( ( aword >> 8 ) & 0xFF );
	}
}

bool MX29LV320E::erase() {
  // Set data pins to output
  dataOut();

  // Chip Erase command sequence
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

bool MX29LV320E::write8(long offset, long page_size, long block_size, byte data[]) {

	// Set data pins to output
	dataOut();

	long lastAddress = offset;
	byte lastData = 0xFF; // nothing written yet (erased state)

	//less than 2MB, use 8bit mode
	for (long bi = 0; bi < block_size; bi+=page_size) {
		// Check if write is complete - poll at the address actually
		// being programmed (the last byte written), against the value
		// actually written there (not a fixed address/value).
		delayMicroseconds(100);
		if (!busyCheck(lastAddress, lastData, PAGE_TIMEOUT_MS)) {
			dataIn();
			return false;
		}

		// Write command sequence
		writeWord(UNLOCK_ADDR_1, 0xaa);
		writeWord(UNLOCK_ADDR_2, 0x55);
		writeWord(UNLOCK_ADDR_1, 0xa0);

		// Write one full page at a time
		for (long pi = 0; pi < page_size; pi++) {
			long a = offset + bi + pi;
			long b = bi + pi;
			writeByte(a, data[b]);
			lastAddress = a;
			lastData = data[b];
		}
	}

	// Check if write is complete
	delayMicroseconds(100);
	bool ok = busyCheck(lastAddress, lastData, PAGE_TIMEOUT_MS);
	// Set data pins to input again
	dataIn();
	return ok;
}

bool MX29LV320E::write16(long offset, long page_size, long block_size, byte data[]) {

	// Set data pins to output
	dataOut();

	long lastAddress = offset/2;
	word lastData = 0xFFFF; // nothing written yet (erased state) - any
	                        // address trivially reads "not busy" against this

	//larger than 2MB, use 16bit mode
	for (long bi = 0; bi < block_size/2; bi+=page_size/2) {
		// Check if write is complete - poll at the address actually
		// being programmed (the last word written), against the value
		// actually written there (not a fixed address/value).
		delayMicroseconds(100);
		if (!busyCheck(lastAddress, lastData, PAGE_TIMEOUT_MS)) {
			dataIn();
			return false;
		}

		// Write command sequence
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
