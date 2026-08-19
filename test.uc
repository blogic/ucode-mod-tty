#!/usr/bin/env ucode

/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */

import * as uloop from 'uloop';
import * as tty from 'tty';

function on_open(port) {
	print("Port opened!\n");
	port.send("Hello from ucode\n");
}

function on_receive(port) {
	let data = port.receive();
	if (length(data) > 0) {
		print("Received: ", data);
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

// Open serial port
let port = tty.open({
	path: "/dev/ttyACM0",
	baud: 4800,
	bits: 8,
	parity: false,
	stopbits: 1,
	on_open,
	on_receive,
	on_error,
	on_close
});

print("Serial port configured. Waiting for events...\n");

// Run the event loop
uloop.run();
