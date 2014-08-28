/*
 * block.c - update of a single status line block
 * Copyright (C) 2014  Vivien Didelot
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "block.h"
#include "click.h"
#include "log.h"

static void
child_setup_env(struct block *block, struct click *click)
{
	if (setenv("BLOCK_NAME", NAME(block), 1) == -1)
		_exit(1);

	if (setenv("BLOCK_INSTANCE", INSTANCE(block), 1) == -1)
		_exit(1);

	if (setenv("BLOCK_BUTTON", click ? click->button : "", 1) == -1)
		_exit(1);

	if (setenv("BLOCK_X", click ? click->x : "", 1) == -1)
		_exit(1);

	if (setenv("BLOCK_Y", click ? click->y : "", 1) == -1)
		_exit(1);
}

static void
child_reset_signals(void)
{
	sigset_t set;

	if (sigfillset(&set) == -1)
		_exit(1);

	/* It should be safe to assume that all signals are unblocked by default */
	if (sigprocmask(SIG_UNBLOCK, &set, NULL) == -1)
		_exit(1);
}

static void
child_redirect_write(int pipe[2], int fd)
{
	if (close(pipe[0]) == -1)
		_exit(1);

	/* Defensive check */
	if (pipe[1] == fd)
		return;

	if (dup2(pipe[1], fd) == -1)
		_exit(1);

	if (close(pipe[1]) == -1)
		_exit(1);
}

static void
linecpy(char **lines, char *dest, unsigned int size)
{
	char *newline = strchr(*lines, '\n');

	/* split if there's a newline */
	if (newline)
		*newline = '\0';

	/* if text in non-empty, copy it */
	if (**lines) {
		strncpy(dest, *lines, size);
		*lines += strlen(dest);
	}

	/* increment if next char is non-null */
	if (*(*lines + 1))
		*lines += 1;
}

static void
mark_as_failed(struct block *block, const char *reason)
{
	struct properties *props = &block->updated_props;

	memset(props, 0, sizeof(struct properties));
	snprintf(props->short_text, sizeof(props->short_text), "[%s] ERROR", NAME(block));
	snprintf(props->full_text, sizeof(props->full_text), "%s %s", props->short_text, reason);
	strcpy(props->color, "#FF0000");
	strcpy(props->urgent, "true");
}

void
block_spawn(struct block *block, struct click *click)
{
	const unsigned long now = time(NULL);
	int out[2], err[2];

	if (!*COMMAND(block)) {
		bdebug(block, "no command, skipping");
		return;
	}

	if (block->pid > 0) {
		bdebug(block, "process already spawned");
		return;
	}

	if (pipe(out) == -1 || pipe(err) == -1) {
		berrorx(block, "pipe");
		return mark_as_failed(block, strerror(errno));
	}

	block->pid = fork();
	if (block->pid == -1) {
		berrorx(block, "fork");
		return mark_as_failed(block, strerror(errno));
	}

	/* Child? */
	if (block->pid == 0) {
		child_setup_env(block, click);
		child_reset_signals();
		child_redirect_write(out, STDOUT_FILENO);
		child_redirect_write(err, STDERR_FILENO);
		execl("/bin/sh", "/bin/sh", "-c", COMMAND(block), (char *) NULL);
		_exit(1); /* Unlikely */
	}

	/*
	 * Note: no need to set the pipe read end as non-blocking, since it is
	 * meant to be read once the child has exited (and thus the write end is
	 * closed and read is available).
	 */

	/* Parent */
	if (close(out[1]) == -1)
		berrorx(block, "close stdout");
	if (close(err[1]) == -1)
		berrorx(block, "close stderr");

	block->out = out[0];
	block->err = err[0];

	if (!click)
		block->timestamp = now;

	bdebug(block, "forked child %d at %ld", block->pid, now);
}

void
block_reap(struct block *block)
{
	struct properties *props = &block->updated_props;

	char buf[2048] = { 0 };
	char *lines = buf;
	int status, code;

	if (block->pid <= 0) {
		bdebug(block, "not spawned yet");
		return;
	}

	if (waitpid(block->pid, &status, 0) == -1) {
		berrorx(block, "waitpid(%d)", block->pid);
		mark_as_failed(block, strerror(errno));
		goto close;
	}

	code = WEXITSTATUS(status);
	bdebug(block, "process %d exited with %d", block->pid, code);

	/* Note read(2) returns 0 for end-of-pipe */
	if (read(block->err, buf, sizeof(buf)) == -1) {
		berrorx(block, "read stderr");
		mark_as_failed(block, strerror(errno));
		goto close;
	}

	if (*buf) {
		bdebug(block, "stderr:\n{\n%s\n}", buf);
		memset(buf, 0, sizeof(buf));
	}

	if (code != 0 && code != '!') {
		char reason[32];

		sprintf(reason, "bad exit code %d", code);
		berror(block, "%s", reason);
		mark_as_failed(block, reason);
		goto close;
	}

	/* Note read(2) returns 0 for end-of-pipe */
	if (read(block->out, buf, sizeof(buf)) == -1) {
		berrorx(block, "read stdout");
		mark_as_failed(block, strerror(errno));
		goto close;
	}

	/* The update went ok, so reset the defaults and merge the output */
	memcpy(props, &block->default_props, sizeof(struct properties));
	strncpy(props->urgent, code == '!' ? "true" : "false", sizeof(props->urgent) - 1);
	linecpy(&lines, props->full_text, sizeof(props->full_text) - 1);
	linecpy(&lines, props->short_text, sizeof(props->short_text) - 1);
	linecpy(&lines, props->color, sizeof(props->color) - 1);

	if (*FULL_TEXT(block) && *LABEL(block)) {
		static const size_t size = sizeof(props->full_text);
		char concat[size];
		snprintf(concat, size, "%s %s", LABEL(block), FULL_TEXT(block));
		strcpy(props->full_text, concat);
	}

	bdebug(block, "updated successfully");
close:
	if (close(block->out) == -1)
		berrorx(block, "close stdout");
	if (close(block->err) == -1)
		berrorx(block, "close stderr");

	block->pid = 0;
}

void block_setup(struct block *block)
{
	struct properties *defaults = &block->default_props;
	struct properties *updated = &block->updated_props;

	/* Convenient shortcuts */
	if (strcmp(defaults->interval, "once") == 0)
		block->interval = INTER_ONCE;
	else if (strcmp(defaults->interval, "repeat") == 0)
		block->interval = INTER_REPEAT;
	else
		block->interval = atoi(defaults->interval);
	block->signal = atoi(defaults->signal);

	/* First update (for static blocks and loading labels) */
	memcpy(updated, defaults, sizeof(struct properties));

#define PLACEHOLDERS(_name, _size, _flags) "    %s: \"%s\"\n"
#define ARGS(_name, _size, _flags) #_name, updated->_name,

	debug("\n{\n" PROPERTIES(PLACEHOLDERS) "%s", PROPERTIES(ARGS) "}");

#undef ARGS
#undef PLACEHOLDERS
}
