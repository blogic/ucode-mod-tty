/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>

#include <libubox/uloop.h>
#include <libubox/ustream.h>

#include <ucode/module.h>

enum {
	SLOT_ON_OPEN,
	SLOT_ON_RECEIVE,
	SLOT_ON_ERROR,
	SLOT_ON_CLOSE,
	SLOT_MAX
};

typedef struct {
	union {
		struct uloop_fd ufd;     /* For raw mode */
		struct ustream_fd sfd;   /* For buffered/line mode */
	};
	uc_value_t *res;  /* Self reference, kept while the port is open */
	uc_vm_t *vm;
	char *path;
	char *line_buffer;  /* Buffer for current line in line-based mode */
	size_t line_len;    /* Length of current line */
	size_t line_max;    /* Drop threshold for a line without terminator */
	int fd;
	struct termios orig_termios;
	bool is_open;
	bool line_based;  /* Line-based mode flag */
	bool use_ustream; /* Whether we're using ustream */
} uc_tty_t;

static void tty_detach(uc_tty_t *tty);

static void tty_free(void *ptr)
{
	uc_tty_t *tty = ptr;

	/* No callback from here, the VM may already be tearing down */
	if (tty->is_open)
		tty_detach(tty);

	free(tty->path);
	free(tty->line_buffer);
}

/*
 * An exception in a callback is handled the way the uloop module handles it,
 * including the handler that uloop.guard() installs. Without that an exit()
 * or a runtime error inside a callback would be swallowed and the event loop
 * would carry on regardless.
 */
static void tty_vm_call(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *handler;

	if (uc_vm_call(vm, false, nargs) == EXCEPTION_NONE) {
		ucv_put(uc_vm_stack_pop(vm));
		return;
	}

	handler = uc_vm_registry_get(vm, "uloop.ex_handler");

	if (ucv_is_callable(handler)) {
		uc_vm_stack_push(vm, ucv_get(handler));
		uc_vm_stack_push(vm, uc_vm_exception_object(vm));

		if (uc_vm_call(vm, false, 1) == EXCEPTION_NONE) {
			ucv_put(uc_vm_stack_pop(vm));
			return;
		}
	}

	uloop_end();
}

static void tty_callback_arg(uc_tty_t *tty, size_t slot, uc_value_t *arg)
{
	uc_value_t *cb;
	size_t nargs = 1;

	if (!tty->res) {
		ucv_put(arg);
		return;
	}

	cb = ucv_resource_value_get(tty->res, slot);
	if (!ucv_is_callable(cb)) {
		ucv_put(arg);
		return;
	}

	uc_vm_stack_push(tty->vm, ucv_get(cb));
	uc_vm_stack_push(tty->vm, ucv_get(tty->res));

	if (arg) {
		uc_vm_stack_push(tty->vm, arg);
		nargs++;
	}

	tty_vm_call(tty->vm, nargs);
}

static void tty_callback(uc_tty_t *tty, size_t slot)
{
	tty_callback_arg(tty, slot, NULL);
}

/* The reason tells a dead port apart from a single unusable line */
static void tty_error(uc_tty_t *tty, const char *reason)
{
	tty_callback_arg(tty, SLOT_ON_ERROR, ucv_string_new(reason));
}

/*
 * ustream_fd_read_pending() keeps reading after notify_read returns, so the
 * stream has to be blocked before the descriptor goes away. Clearing the
 * descriptor number as well stops a later re-registration from picking up an
 * unrelated file that happens to reuse the number.
 */
static void tty_detach(uc_tty_t *tty)
{
	if (tty->use_ustream) {
		ustream_set_read_blocked(&tty->sfd.stream, true);
		ustream_free(&tty->sfd.stream);
		tty->sfd.fd.fd = -1;
	} else {
		uloop_fd_delete(&tty->ufd);
		tty->ufd.fd = -1;
	}

	tcsetattr(tty->fd, TCSANOW, &tty->orig_termios);
	close(tty->fd);
	tty->fd = -1;
	tty->is_open = false;
}

/* May free the port, so the caller has to hold a reference of its own */
static void tty_shutdown(uc_tty_t *tty)
{
	uc_value_t *res = tty->res;

	tty_detach(tty);
	tty_callback(tty, SLOT_ON_CLOSE);

	tty->res = NULL;

	if (res) {
		ucv_resource_persistent_set(res, false);
		ucv_put(res);
	}
}

static void tty_slot_set(uc_value_t *res, size_t slot, uc_value_t *cb)
{
	if (ucv_is_callable(cb))
		ucv_resource_value_set(res, slot, ucv_get(cb));
}

static void tty_line_store(uc_tty_t *tty, const char *line, size_t len)
{
	free(tty->line_buffer);
	tty->line_len = 0;

	tty->line_buffer = malloc(len + 1);
	if (!tty->line_buffer)
		return;

	memcpy(tty->line_buffer, line, len);
	tty->line_buffer[len] = '\0';
	tty->line_len = len;
}

/*
 * ustream calls notify_read once per read() syscall, not once per pending
 * line, so a single chunk holding several lines has to be drained here. A
 * callback that only handled the first line would leave the rest queued until
 * the next chunk arrived and the read buffer would eventually block.
 */
static void tty_ustream_read_cb(struct ustream *s, int bytes_new)
{
	uc_tty_t *tty = container_of(s, uc_tty_t, sfd.stream);
	uc_value_t *res = ucv_get(tty->res);  /* Survive a close() from a callback */
	char *data, *newline;
	int len;

	while (tty->is_open) {
		data = ustream_get_read_buf(s, &len);
		if (!data || len == 0)
			break;

		newline = memchr(data, '\n', len);
		if (!newline) {
			if ((size_t)len < tty->line_max)
				break;

			/* Without this the stream stays full and reads stop for good */
			ustream_consume(s, len);
			tty_error(tty, "overflow");
			continue;
		}

		tty_line_store(tty, data, newline - data);
		ustream_consume(s, newline - data + 1);
		tty_callback(tty, SLOT_ON_RECEIVE);
	}

	ucv_put(res);
}

/* ustream notify_state callback - called on EOF or errors */
static void tty_ustream_state_cb(struct ustream *s)
{
	uc_tty_t *tty = container_of(s, uc_tty_t, sfd.stream);

	if (!tty->is_open)
		return;

	if (s->eof)
		tty_error(tty, "eof");
	else if (s->write_error)
		tty_error(tty, "error");
}

/* Raw uloop callback for non-buffered mode */
static void tty_uloop_cb(struct uloop_fd *u, unsigned int events)
{
	uc_tty_t *tty = container_of(u, uc_tty_t, ufd);
	uc_value_t *res = ucv_get(tty->res);  /* Survive a close() from a callback */

	if (events & ULOOP_READ)
		tty_callback(tty, SLOT_ON_RECEIVE);

	if (tty->is_open && (events & ULOOP_ERROR_CB))
		tty_error(tty, "error");

	ucv_put(res);
}

static bool tty_speed_get(int baud, speed_t *speed_out)
{
	speed_t speed;

	switch (baud) {
	case 50: speed = B50; break;
	case 75: speed = B75; break;
	case 110: speed = B110; break;
	case 134: speed = B134; break;
	case 150: speed = B150; break;
	case 200: speed = B200; break;
	case 300: speed = B300; break;
	case 600: speed = B600; break;
	case 1200: speed = B1200; break;
	case 1800: speed = B1800; break;
	case 2400: speed = B2400; break;
	case 4800: speed = B4800; break;
	case 9600: speed = B9600; break;
	case 19200: speed = B19200; break;
	case 38400: speed = B38400; break;
	case 57600: speed = B57600; break;
	case 115200: speed = B115200; break;
	case 230400: speed = B230400; break;
	case 460800: speed = B460800; break;
	case 500000: speed = B500000; break;
	case 576000: speed = B576000; break;
	case 921600: speed = B921600; break;
	case 1000000: speed = B1000000; break;
	case 1152000: speed = B1152000; break;
	case 1500000: speed = B1500000; break;
	case 2000000: speed = B2000000; break;
	case 2500000: speed = B2500000; break;
	case 3000000: speed = B3000000; break;
	case 3500000: speed = B3500000; break;
	case 4000000: speed = B4000000; break;
	default: return false;
	}

	*speed_out = speed;

	return true;
}

static int configure_tty(int fd, speed_t speed, int bits, int parity, int stop_bits, bool rtscts)
{
	struct termios tio;

	if (tcgetattr(fd, &tio) != 0)
		return -1;

	/* Pass the data through untouched, both directions */
	cfmakeraw(&tio);

	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);

	/* CLOCAL, because a modem control change must not hang up the port */
	tio.c_cflag |= CREAD | CLOCAL;

	tio.c_cflag &= ~CSIZE;
	switch (bits) {
	case 5: tio.c_cflag |= CS5; break;
	case 6: tio.c_cflag |= CS6; break;
	case 7: tio.c_cflag |= CS7; break;
	default: tio.c_cflag |= CS8; break;
	}

	if (parity) {
		tio.c_cflag |= PARENB;

		if (parity == 2)
			tio.c_cflag |= PARODD;
		else
			tio.c_cflag &= ~PARODD;

		/* Check the parity bit rather than ignore what it reports */
		tio.c_iflag |= INPCK;
		tio.c_iflag &= ~IGNPAR;
	} else {
		tio.c_cflag &= ~PARENB;
		tio.c_iflag &= ~INPCK;
	}

	if (stop_bits == 2)
		tio.c_cflag |= CSTOPB;
	else
		tio.c_cflag &= ~CSTOPB;

	if (rtscts)
		tio.c_cflag |= CRTSCTS;
	else
		tio.c_cflag &= ~CRTSCTS;

	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &tio) != 0)
		return -1;

	return 0;
}

static uc_value_t *uc_tty_open(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *config = uc_fn_arg(0);
	uc_value_t *path_val, *baud_val, *bits_val, *parity_val, *stop_val;
	uc_value_t *on_open_val, *on_receive_val, *on_error_val, *on_close_val;
	uc_value_t *linebased_val, *linemax_val, *rtscts_val, *res;
	struct termios orig_termios;
	uc_tty_t *tty;
	const char *path;
	speed_t speed;
	int baud = 9600, bits = 8, parity = 0, stop_bits = 1, fd;
	size_t line_max = 4096;
	bool line_based = false, rtscts = false;

	if (ucv_type(config) != UC_OBJECT) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Expecting object argument");
		return NULL;
	}

	path_val = ucv_object_get(config, "path", NULL);
	if (!path_val || ucv_type(path_val) != UC_STRING) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Missing or invalid 'path' parameter");
		return NULL;
	}
	path = ucv_string_get(path_val);

	baud_val = ucv_object_get(config, "baud", NULL);
	if (baud_val) {
		baud = ucv_int64_get(baud_val);
	}

	bits_val = ucv_object_get(config, "bits", NULL);
	if (bits_val) {
		bits = ucv_int64_get(bits_val);
	}

	parity_val = ucv_object_get(config, "parity", NULL);
	if (parity_val) {
		if (ucv_type(parity_val) == UC_BOOLEAN) {
			parity = ucv_boolean_get(parity_val) ? 1 : 0;
		} else if (ucv_type(parity_val) == UC_STRING) {
			const char *p = ucv_string_get(parity_val);
			if (!strcmp(p, "none")) parity = 0;
			else if (!strcmp(p, "even")) parity = 1;
			else if (!strcmp(p, "odd")) parity = 2;
			else parity = -1;
		} else {
			parity = ucv_int64_get(parity_val);
		}
	}

	stop_val = ucv_object_get(config, "stopbits", NULL);
	if (!stop_val) {
		stop_val = ucv_object_get(config, "stopbit", NULL);
	}
	if (stop_val) {
		stop_bits = ucv_int64_get(stop_val);
	}

	rtscts_val = ucv_object_get(config, "rtscts", NULL);
	if (rtscts_val) {
		rtscts = ucv_is_truish(rtscts_val);
	}

	if (!tty_speed_get(baud, &speed)) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Unsupported baud rate %d", baud);
		return NULL;
	}

	if (bits < 5 || bits > 8) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid 'bits' parameter %d", bits);
		return NULL;
	}

	if (parity < 0 || parity > 2) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid 'parity' parameter");
		return NULL;
	}

	if (stop_bits < 1 || stop_bits > 2) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid 'stopbits' parameter %d", stop_bits);
		return NULL;
	}

	on_open_val = ucv_object_get(config, "on_open", NULL);
	on_receive_val = ucv_object_get(config, "on_receive", NULL);
	on_error_val = ucv_object_get(config, "on_error", NULL);
	on_close_val = ucv_object_get(config, "on_close", NULL);

	/* Check for line-based mode */
	linebased_val = ucv_object_get(config, "linebased", NULL);
	if (linebased_val) {
		line_based = ucv_is_truish(linebased_val);
	}

	linemax_val = ucv_object_get(config, "linemax", NULL);
	if (linemax_val) {
		int64_t max = ucv_int64_get(linemax_val);

		if (max < 1 || max > (1 << 20)) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid 'linemax' parameter");
			return NULL;
		}

		line_max = max;
	}

	fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Failed to open %s: %s", path, strerror(errno));
		return NULL;
	}

	if (tcgetattr(fd, &orig_termios) != 0) {
		close(fd);
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Failed to get terminal attributes");
		return NULL;
	}

	if (configure_tty(fd, speed, bits, parity, stop_bits, rtscts) != 0) {
		close(fd);
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Failed to configure serial port");
		return NULL;
	}

	res = ucv_resource_create_ex(vm, "tty.port", (void **)&tty, SLOT_MAX, sizeof(*tty));
	if (!res) {
		tcsetattr(fd, TCSANOW, &orig_termios);
		close(fd);
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Out of memory");
		return NULL;
	}

	tty->vm = vm;
	tty->fd = fd;
	tty->orig_termios = orig_termios;
	tty->path = strdup(path);
	tty->line_based = line_based;
	tty->line_max = line_max;

	if (line_based) {
		/* Use ustream for line-based mode */
		tty->use_ustream = true;
		tty->sfd.stream.string_data = true;
		tty->sfd.stream.notify_read = tty_ustream_read_cb;
		tty->sfd.stream.notify_state = tty_ustream_state_cb;
		/* Sized so that a full read buffer is exactly an overlong line */
		tty->sfd.stream.r.buffer_len = line_max;
		ustream_fd_init(&tty->sfd, fd);
	} else {
		/* Use raw uloop for non-buffered mode */
		tty->use_ustream = false;
		tty->ufd.fd = fd;
		tty->ufd.cb = tty_uloop_cb;

		int ret = uloop_fd_add(&tty->ufd, ULOOP_READ);
		if (ret != 0) {
			tcsetattr(fd, TCSANOW, &orig_termios);
			close(fd);
			tty->fd = -1;
			ucv_put(res);
			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Failed to add fd to uloop: %d", ret);
			return NULL;
		}
	}

	/* Flush input after uloop setup */
	tcflush(fd, TCIFLUSH);

	tty->is_open = true;

	/*
	 * uloop owns the port from here on, so it has to survive both the garbage
	 * collector and a script that keeps no reference of its own. close()
	 * releases this again.
	 */
	tty->res = ucv_get(res);
	ucv_resource_persistent_set(res, true);

	tty_slot_set(res, SLOT_ON_OPEN, on_open_val);
	tty_slot_set(res, SLOT_ON_RECEIVE, on_receive_val);
	tty_slot_set(res, SLOT_ON_ERROR, on_error_val);
	tty_slot_set(res, SLOT_ON_CLOSE, on_close_val);

	tty_callback(tty, SLOT_ON_OPEN);

	return res;
}

static uc_value_t *uc_tty_send(uc_vm_t *vm, size_t nargs)
{
	uc_tty_t *tty = uc_fn_thisval("tty.port");
	uc_value_t *data = uc_fn_arg(0);
	const char *str;
	size_t len, total = 0;
	ssize_t written;

	if (!tty) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid tty port context");
		return NULL;
	}

	if (!tty->is_open) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Port not open");
		return NULL;
	}

	if (ucv_type(data) != UC_STRING) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Expected string argument");
		return NULL;
	}

	str = ucv_string_get(data);
	len = ucv_string_length(data);  /* strlen() would stop at an embedded null */

	/*
	 * The descriptor is non blocking, so a full buffer is not an error. Report
	 * how much went out and leave the rest to the caller.
	 */
	while (total < len) {
		written = write(tty->fd, str + total, len - total);

		if (written < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;

			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Write failed: %s", strerror(errno));
			return NULL;
		}

		total += written;
	}

	return ucv_int64_new(total);
}

static uc_value_t *uc_tty_receive(uc_vm_t *vm, size_t nargs)
{
	uc_tty_t *tty = uc_fn_thisval("tty.port");
	uc_value_t *maxlen_val = uc_fn_arg(0);
	char buffer[4096];
	size_t maxlen = sizeof(buffer);
	ssize_t nread;

	if (!tty) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid tty port context");
		return NULL;
	}

	if (!tty->is_open) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Port not open");
		return NULL;
	}

	if (tty->use_ustream && tty->line_based) {
		/* In line-based mode, return the buffered line */
		if (tty->line_buffer && tty->line_len > 0) {
			uc_value_t *result = ucv_string_new_length(tty->line_buffer, tty->line_len);
			/* Clear the buffer after reading */
			free(tty->line_buffer);
			tty->line_buffer = NULL;
			tty->line_len = 0;
			return result;
		}
		return ucv_string_new("");
	}

	if (maxlen_val) {
		int64_t requested = ucv_int64_get(maxlen_val);

		if (requested < 1) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid maximum length argument");
			return NULL;
		}

		if ((size_t)requested < maxlen)
			maxlen = requested;
	}

	nread = read(tty->fd, buffer, maxlen);
	if (nread < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return ucv_string_new("");
		}
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Read failed: %s", strerror(errno));
		return NULL;
	}

	return ucv_string_new_length(buffer, nread);
}

static uc_value_t *uc_tty_close(uc_vm_t *vm, size_t nargs)
{
	uc_tty_t *tty = uc_fn_thisval("tty.port");

	if (!tty) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid tty port context");
		return NULL;
	}

	if (tty->is_open)
		tty_shutdown(tty);

	return ucv_boolean_new(true);
}

static uc_value_t *uc_tty_flush(uc_vm_t *vm, size_t nargs)
{
	uc_tty_t *tty = uc_fn_thisval("tty.port");

	if (!tty) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid tty port context");
		return NULL;
	}

	if (!tty->is_open) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Port not open");
		return NULL;
	}

	if (tcdrain(tty->fd) != 0) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Flush failed: %s", strerror(errno));
		return NULL;
	}

	return ucv_boolean_new(true);
}

static uc_value_t *uc_tty_set_dtr(uc_vm_t *vm, size_t nargs)
{
	uc_tty_t *tty = uc_fn_thisval("tty.port");
	uc_value_t *state_val = uc_fn_arg(0);
	int status;
	bool state = true;

	if (!tty) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid tty port context");
		return NULL;
	}

	if (!tty->is_open) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Port not open");
		return NULL;
	}

	if (state_val) {
		state = ucv_is_truish(state_val);
	}

	if (ioctl(tty->fd, TIOCMGET, &status) < 0) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Failed to get modem status");
		return NULL;
	}

	if (state) {
		status |= TIOCM_DTR;
	} else {
		status &= ~TIOCM_DTR;
	}

	if (ioctl(tty->fd, TIOCMSET, &status) < 0) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Failed to set DTR");
		return NULL;
	}

	return ucv_boolean_new(true);
}

static uc_value_t *uc_tty_set_rts(uc_vm_t *vm, size_t nargs)
{
	uc_tty_t *tty = uc_fn_thisval("tty.port");
	uc_value_t *state_val = uc_fn_arg(0);
	int status;
	bool state = true;

	if (!tty) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid tty port context");
		return NULL;
	}

	if (!tty->is_open) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Port not open");
		return NULL;
	}

	if (state_val) {
		state = ucv_is_truish(state_val);
	}

	if (ioctl(tty->fd, TIOCMGET, &status) < 0) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Failed to get modem status");
		return NULL;
	}

	if (state) {
		status |= TIOCM_RTS;
	} else {
		status &= ~TIOCM_RTS;
	}

	if (ioctl(tty->fd, TIOCMSET, &status) < 0) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Failed to set RTS");
		return NULL;
	}

	return ucv_boolean_new(true);
}

static const uc_function_list_t tty_port_fns[] = {
	{ "send", uc_tty_send },
	{ "receive", uc_tty_receive },
	{ "close", uc_tty_close },
	{ "flush", uc_tty_flush },
	{ "set_dtr", uc_tty_set_dtr },
	{ "set_rts", uc_tty_set_rts },
};

static const uc_function_list_t global_fns[] = {
	{ "open", uc_tty_open },
};

static void register_functions(uc_vm_t *vm, uc_value_t *scope)
{
	uc_function_list_register(scope, global_fns);
}

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	uc_type_declare(vm, "tty.port", tty_port_fns, tty_free);
	register_functions(vm, scope);
}