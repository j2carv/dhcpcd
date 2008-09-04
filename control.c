/* 
 * dhcpcd - DHCP client daemon
 * Copyright 2006-2008 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/stat.h>
#include <sys/un.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "common.h"
#include "control.h"
#include "eloop.h"
#include "dhcpcd.h"

static int fd = -1;
struct sockaddr_un sun;
static char buffer[1024];
static char *argvp[255];

static int fds[5];

static void
remove_control_data(void *arg)
{
	size_t i;

	for (i = 0; i < sizeof(fds); i++) {
		if (&fds[i] == arg) {
			close(fds[i]);
			delete_event(fds[i]);
			fds[i] = -1;
		}
	}
}

static void
handle_control_data(void *arg)
{
	ssize_t bytes;
	int argc, *s = arg;
	char *e, *p;
	char **ap;

	for (;;) {
		bytes = read(*s, buffer, sizeof(buffer));
		if (bytes == -1 || bytes == 0) {
			remove_control_data(arg);
			return;
		}
		p = buffer;
		e = buffer + bytes;
		argc = 0;
		ap = argvp;
		while (p < e && (size_t)argc < sizeof(argvp)) {
			argc++;
			*ap++ = p;
			p += strlen(p) + 1;
		}
		handle_args(argc, argvp);
	}
}

static void
handle_control(_unused void *arg)
{
	struct sockaddr_un run;
	socklen_t len;
	size_t i;

	for (i = 0; i < sizeof(fds); i++) {
		if (fds[i] == -1)
			break;
	}
	if (i >= sizeof(fds))
		return;

	len = sizeof(run);
	if ((fds[i] = accept(fd, (struct sockaddr *)&run, &len)) == -1)
		return;
	add_event(fds[i], handle_control_data, &fds[i]);
	/* Timeout the connection after 5 minutes - should be plenty */
	add_timeout_sec(300, remove_control_data, &fds[i]);
}

static int
make_sock(void)
{
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, CONTROLSOCKET, sizeof(sun.sun_path));
	return sizeof(sun.sun_family) + strlen(sun.sun_path) + 1;
}

int
start_control(void)
{
	int len;

	if ((len = make_sock()) == -1)
		return -1;
	unlink(CONTROLSOCKET);
	if (bind(fd, (struct sockaddr *)&sun, len) == -1 ||
	    chmod(CONTROLSOCKET, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) == -1 ||
	    set_cloexec(fd) == -1 ||
	    set_nonblock(fd) == -1 ||
	    listen(fd, sizeof(fds)) == -1)
	{
		close(fd);
		return -1;
	}
	memset(fds, -1, sizeof(fds));
	add_event(fd, handle_control, NULL);
	return fd;
}

int
stop_control(void)
{
	int retval = 0;
	if (close(fd) == -1)
		retval = 1;
	if (unlink(CONTROLSOCKET) == -1)
		retval = -1;
	return retval;
}

int
open_control(void)
{
	int len;

	if ((len = make_sock()) == -1)
		return -1;
	return connect(fd, (struct sockaddr *)&sun, len);
}

int
send_control(int argc, char * const *argv)
{
	char *p = buffer;
	int i;
	size_t len;

	if (argc > 255) {
		errno = ENOBUFS;
		return -1;
	}
	for (i = 0; i < argc; i++) {
		len = strlen(argv[i]) + 1;
		if ((p - buffer) + len > sizeof(buffer)) {
			errno = ENOBUFS;
			return -1;
		}
		memcpy(p, argv[i], len);
		p += len;
	}
	return write(fd, buffer, p - buffer);
}