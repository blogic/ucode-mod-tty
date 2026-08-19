#!/usr/bin/env ucode

/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */

import * as uloop from 'uloop';
import * as tty from 'tty';

let line_count = 0;

function on_open(port) {
	print("Port opened with line-based mode!\n");
}

function on_receive(port) {
	let line = port.receive();
	if (length(line) > 0) {
		line_count++;
		print(`Line ${line_count}: ${line}\n`);
	}
}

function on_error(port) {
	print("Port error!\n");
}

function on_close(port) {
	print("Port closed!\n");
}

// Initialize uloop first
uloop.init();

// Open serial port with line-based mode
let port = tty.open({
	path: "/dev/ttyACM0",
	baud: 4800,
	bits: 8,
	parity: false,
	stopbits: 1,
	linebased: true,  // Enable line-based mode
	on_open,
	on_receive,
	on_error,
	on_close
});

print("Serial port configured with line-based mode. Waiting for NMEA lines...\n");

// Run the event loop
uloop.run();
