/**
 * Mega Burner
 * A EEPROM programmer based on Arduino Mega 2560.
 *
 * Version 1.0
 * Date 2017.11.11
 * Contact: mingzo@gmail.com
 */
#include "MegaBurner.h"
#include "FlashChip.h"
#include "MX29L3211.h"
#include "MX29LV320E.h"
#include "MX26L6420.h"

// ---- Chip selection (runtime) ----
// All supported chip drivers are instantiated up front; the host
// selects which one is physically connected with the "S<name>"
// command (see selectChip() below) instead of needing a firmware
// reflash per chip. activeChip defaults to MX29L3211 so existing
// host code that never sends "S" keeps working unchanged.
//
// MX29LV320E covers BOTH Top-Boot (MX29LV320ET...) and Bottom-Boot
// (MX29LV320EB...) variants - see MX29LV320E.h for why one firmware
// class covers both; the host selects "MX29LV320E" for either.
MX29L3211  mx29l3211  = MX29L3211();
MX29LV320E mx29lv320e = MX29LV320E();
MX26L6420  mx26l6420  = MX26L6420();

FlashChip* activeChip = &mx29l3211;

// Command is "S<name>", e.g. "SMX29LV320E". Replies with '%' when
// the newly-selected chip driver has finished its init() (which
// itself does an id-read + reset against whatever chip is now
// physically present). Unknown names are ignored (activeChip is left
// unchanged) rather than silently picking something - a wrong guess
// here could mean issuing another chip's command sequence.
void selectChip(String name) {
	if (name == "MX29L3211") {
		activeChip = &mx29l3211;
	} else if (name == "MX29LV320E") {
		activeChip = &mx29lv320e;
	} else if (name == "MX26L6420") {
		activeChip = &mx26l6420;
	} else {
		return;
	}

	activeChip->init();
	Serial.println('%');
}

/*
 * Identify this device to the host
 *
 * Command is "P". Replies with "megaburner" so the host can scan
 * every available serial port and find the Arduino automatically,
 * instead of the user having to hard-code a COM port number.
 *
 * Deliberately a distinct command from anything that touches the
 * chip: a ping must be completely harmless to send to an unknown
 * device (the host will be probing ports that might be a mouse, a
 * modem, another microcontroller...), and equally harmless to
 * receive here regardless of which chip is selected or whether one
 * is even connected.
 */
void ping() {
	Serial.println("megaburner");
}

// LED activity indicators
// D13 lights up whenever data is being WRITTEN TO the chip (write + erase)
// D12 lights up whenever data is being READ FROM the chip
#define WRITE_LED_PIN 12
#define READ_LED_PIN  13
#define STARTUP_FLASH_COUNT 10
#define STARTUP_FLASH_MS    50

/*
 * Check the id of the chip
 *
 * Command is "C"
 */
void check() {
	activeChip->readId();
	activeChip->reset();
}

/*
 * Read operation
 *
 * the total capacity is divided into several blocks, client reads one block at a time.
 * Command example: R0,4096 (indicates the 1st block, from 0 to 4095 byte )
 *
 */
void read(String param) {
	long block_id  = split(param, ',', 0).toInt();
	long block_size = split(param, ',', 1).toInt();

	digitalWrite(READ_LED_PIN, HIGH);

	activeChip->reset();
	activeChip->read16(block_id, block_size);

	digitalWrite(READ_LED_PIN, LOW);
}

/*
 * Erase operation
 *
 * Command is "E"
 */
void erase() {
	digitalWrite(WRITE_LED_PIN, HIGH);

	activeChip->reset();
	bool ok = activeChip->erase();

	// '%' = success, '!' = the chip never reported completion within
	// a sane timeout (see busyCheck() in the active chip driver) -
	// distinct from '%' so the host can tell a real failure apart from
	// a successful operation, rather than either hanging forever or
	// silently reporting success on a failed erase.
	Serial.println(ok ? '%' : '!');

	digitalWrite(WRITE_LED_PIN, LOW);
}

/*
 * Write operation
 *
 * The EEPROM should support page programming.
 * Command example: W1048576,128,4096 (means write 4096bytes start at 1048576, 128bytes per page)
 */
void write(String param) {
	long offset = split(param, ',', 0).toInt();
	long page_size = split(param, ',', 1).toInt();
	long block_size = split(param, ',', 2).toInt();

	digitalWrite(WRITE_LED_PIN, HIGH);

	//receive incoming data to write
	byte buffer[block_size];

	Serial.println('&');
	long idx = 0;
    while(idx < block_size) {
      if(Serial.available()) {
    	  buffer[idx++] = Serial.read();
      }
    }

	if (block_size < page_size)
		page_size = block_size;

    bool ok = activeChip->write16(offset, page_size, block_size, buffer);
    // '%' = success, '!' = the chip never reported completion within
    // a sane timeout (see busyCheck() in the active chip driver).
    Serial.println(ok ? '%' : '!');

	delay(100);
	activeChip->reset();

	digitalWrite(WRITE_LED_PIN, LOW);
}


void setup() {
	Serial.begin(115200);

	pinMode(WRITE_LED_PIN, OUTPUT);
	pinMode(READ_LED_PIN, OUTPUT);
	digitalWrite(WRITE_LED_PIN, LOW);
	digitalWrite(READ_LED_PIN, LOW);

	// Startup "I'm alive" signal: flash both LEDs together, briefly, 10 times
	for (int i = 0; i < STARTUP_FLASH_COUNT; i++) {
		digitalWrite(WRITE_LED_PIN, HIGH);
		digitalWrite(READ_LED_PIN, HIGH);
		delay(STARTUP_FLASH_MS);
		digitalWrite(WRITE_LED_PIN, LOW);
		digitalWrite(READ_LED_PIN, LOW);
		delay(STARTUP_FLASH_MS);
	}

	activeChip->init();

	// init() unconditionally sends the chip's id over serial
	// (readId(), unsolicited - not a response to any host command). Any
	// bytes a host sent while we were still booting are also sitting in
	// the input buffer at this point. Discard everything now so the
	// first command loop() sees after this is clean, regardless of how
	// long the host waited before sending it.
	while (Serial.available()) {
		Serial.read();
	}
}

void loop() {

	String COMMAND_DATA = "";

	while (Serial.available()) {
		COMMAND_DATA += (char)Serial.read();
	    delay(10);
	}

	if (COMMAND_DATA.length() > 0) {
		switch (COMMAND_DATA[0]) {
			case 'C':
				check();
				break;
			case 'R':
				read(COMMAND_DATA.substring(1));
				break;
			case 'E':
				erase();
				break;
			case 'W':
				write(COMMAND_DATA.substring(1));
				break;
			case 'S':
				selectChip(COMMAND_DATA.substring(1));
				break;
			case 'P':
				ping();
				break;
			default:
				break;
		}
	}

}
