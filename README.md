# ucode-mod-tty

OpenWrt ucode binding for serial port communication using libubox/uloop.

## Features

- Asynchronous serial port I/O driven by the uloop event loop
- Line based buffering for text protocols
- Raw mode for binary protocols
- Hardware flow control (RTS/CTS)
- Configurable baud rates from 50 to 4000000
- 5 to 8 data bits, odd, even or no parity, 1 or 2 stop bits
- Event driven callbacks for open, receive, error and close

## Installation

```bash
cmake .
make
make install
```

The build writes `tty.so` into the source directory and `make install` copies it
to `<prefix>/lib/ucode`.

## API Reference

### `tty.open(config)`

Opens a serial port with the given configuration.

**Parameters:**
- `config` (object): Port configuration
  - `path` (string, required): Device path, for example `/dev/ttyUSB0`
  - `baud` (number): Baud rate, default 9600. See the list below
  - `bits` (number): Data bits, 5 to 8, default 8
  - `parity` (boolean|string|number): Parity, default false
    - `false`, `0` or `"none"`: no parity
    - `true`, `1` or `"even"`: even parity
    - `2` or `"odd"`: odd parity
  - `stopbits` (number): Stop bits, 1 or 2, default 1. `stopbit` is accepted as
    an alias
  - `rtscts` (boolean): Hardware flow control, default false
  - `linebased` (boolean): Line based buffering, default false
  - `linemax` (number): Longest line accepted in line based mode, default 4096,
    maximum 1048576
  - `on_open` (function): Called when the port is open
  - `on_receive` (function): Called when data arrives
  - `on_error` (function): Called on a port error
  - `on_close` (function): Called when the port closes

**Returns:** the port object. Any failure raises an exception, it never returns
null, so wrap the call in `try`/`catch`.

The module holds a reference while the port is open, so the script does not have
to keep one. `close()` releases it.

### Port Methods

#### `port.send(data)`
Sends data to the serial port. The data may contain null bytes.

**Parameters:**
- `data` (string): Data to send

**Returns:** the number of bytes written. Writes are non blocking, so a full
transmit buffer gives a short count.

#### `port.receive([maxlen])`
Reads from the port.

**Parameters:**
- `maxlen` (number, optional): Maximum bytes to read, 1 to 4096, default 4096.
  Ignored in line based mode

**Returns:** a string with the data received, or the empty string when there is
none.

In line based mode this returns the line that `on_receive` was called for.

#### `port.close()`
Closes the serial port, restores the original terminal settings and fires
`on_close`.

**Returns:** true

#### `port.flush()`
Waits until everything queued has been transmitted.

**Returns:** true

#### `port.set_dtr(state)`
Sets the DTR signal.

**Parameters:**
- `state` (boolean): Signal state

**Returns:** true

#### `port.set_rts(state)`
Sets the RTS signal.

**Parameters:**
- `state` (boolean): Signal state

**Returns:** true

## Callbacks

Every callback takes the port as its first argument. Only `on_error` takes a
second one.

### `on_open(port)`
Called from `tty.open()` once the port is ready.

### `on_receive(port)`
Called when there is data to read. Use `port.receive()` to get it.

In **raw mode** this fires whenever data arrives.

In **line based mode** this fires once per complete line, and it fires again
straight away while further complete lines are buffered.

### `on_error(port, reason)`
Called when the port runs into trouble:

- `"eof"` - the far end went away
- `"error"` - the port reported an error
- `"overflow"` - `linemax` bytes arrived without a newline and were discarded

The first two are fatal, the port is gone. The third is not.

### `on_close(port)`
Called when the port closes. Closing from inside a callback is allowed.

## Operating Modes

### Raw Mode (default)
- Data is delivered as it arrives
- No character translation in either direction and no software flow control, so
  binary data passes through unchanged
- `on_receive` fires while data is pending, so a callback that reads only part
  of it is called again

### Line Based Mode
- Enabled with `linebased: true`
- Buffers incoming data until a newline arrives
- `on_receive` fires once per complete line and `receive()` returns that line
  without the trailing newline. A carriage return before it is kept, strip it in
  the script if the protocol uses CR LF
- Every buffered line is delivered before the callback returns to the event loop
- A run of `linemax` bytes without a newline is discarded and `on_error` fires

## Example Scripts

`test.uc` opens a port and does basic I/O. `test_linebased.uc` reads a text
protocol line by line.

## Supported Baud Rates

50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800, 9600, 19200,
38400, 57600, 115200, 230400, 460800, 500000, 576000, 921600, 1000000, 1152000,
1500000, 2000000, 2500000, 3000000, 3500000, 4000000

Any other value raises an exception.

## Requirements

- OpenWrt with ucode support
- libubox
- libucode

## Licence

GPL 2.0 only, see `LICENSE.txt`. Every source file carries an
`SPDX-License-Identifier: GPL-2.0-only` header. Copyright (C) 2026 John
Crispin <john@phrozen.org>.
