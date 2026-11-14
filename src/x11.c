/*
 * X11 applications.
 *
 * tw speaks no X11. xwayland-satellite does: it runs Xwayland and shows each
 * X11 window to tw as an ordinary Wayland window. Both are started only when
 * the first X11 application connects, the way niri does it: tw owns the X11
 * display sockets, watches them, and hands them to the helper when a client
 * turns up.
 *
 * Cost while no X11 application has run: two listening sockets and two
 * entries in the event loop. No process, no wakeups. Once one has run, the
 * helper and Xwayland stay until they exit or the session ends, and tw is told
 * of that exit through a pidfd: one wakeup, no polling, no SIGCHLD handler.
 */
#define _GNU_SOURCE /* syscall(), for pidfd_open */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

#define SOCKET_DIR "/tmp/.X11-unix"
#define MAX_DISPLAY 32

extern char **environ;

struct tw_x11 {
	struct tw_server *server;
	int display;
	char display_name[16]; /* ":<display>" */
	char *program;         /* the helper, resolved once so exec needs no PATH search */
	int fds[2];            /* listening sockets: the file in SOCKET_DIR, then the abstract one */
	/* Watching for a client. NULL while the helper runs: the sockets are its
	 * business then, and a connection it has not accepted yet would keep
	 * waking us. */
	struct wl_event_source *watch[2];
	pid_t pid; /* the running helper, or 0 */
	int pidfd;
	struct wl_event_source *exit_source;
};

/* --- The display sockets ------------------------------------------------------ */

static void lock_path(char *path, size_t size, int display) {
	snprintf(path, size, "/tmp/.X%d-lock", display);
}

/* X servers refuse a socket directory that another user could swap out. */
static bool socket_dir_ok(void) {
	if (mkdir(SOCKET_DIR, 01777) == 0) {
		/* World-writable and sticky like /tmp, whatever the umask says, so
		 * that other users' X servers can still use it. */
		chmod(SOCKET_DIR, 01777);
		return true;
	}
	struct stat st;
	if (errno != EEXIST || lstat(SOCKET_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return false;
	}
	if (st.st_uid != 0 && st.st_uid != getuid()) {
		return false;
	}
	return (st.st_mode & S_ISVTX) || !(st.st_mode & (S_IWGRP | S_IWOTH));
}

/* Claims /tmp/.X<display>-lock the way X servers do: created exclusively,
 * holding our pid. A lock left behind by a dead process is taken over. */
static bool lock_display(int display) {
	char path[32], text[16];
	lock_path(path, sizeof(path), display);
	for (int attempt = 0; attempt < 2; attempt++) {
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
		if (fd >= 0) {
			int length = snprintf(text, sizeof(text), "%10d\n", (int)getpid());
			bool written = write(fd, text, (size_t)length) == length;
			close(fd);
			if (!written) {
				unlink(path);
			}
			return written;
		}
		if (errno != EEXIST || (fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
			return false;
		}
		ssize_t bytes = read(fd, text, sizeof(text) - 1);
		close(fd);
		if (bytes <= 0) {
			return false;
		}
		text[bytes] = '\0';
		char *end;
		long owner = strtol(text, &end, 10);
		if (end == text || owner <= 0 || (kill((pid_t)owner, 0) == 0 || errno != ESRCH)) {
			return false; /* in use, or not ours to judge */
		}
		if (unlink(path) != 0) {
			return false;
		}
	}
	return false;
}

static int listen_on(const struct sockaddr_un *addr, socklen_t length) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	/* Clients that connect while Xwayland is still starting wait in this
	 * queue, so it has to hold more than a couple. */
	if (bind(fd, (const struct sockaddr *)addr, length) < 0 || listen(fd, 128) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool open_sockets(struct tw_x11 *x11, int display) {
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	socklen_t base = offsetof(struct sockaddr_un, sun_path);

	/* The path clients look for first, and the same name in Linux's abstract
	 * namespace, which reaches into sandboxes that hide /tmp. */
	int length = snprintf(addr.sun_path, sizeof(addr.sun_path), SOCKET_DIR "/X%d", display);
	unlink(addr.sun_path); /* we hold the lock, so a leftover is stale */
	x11->fds[0] = listen_on(&addr, base + (socklen_t)length + 1);
	if (x11->fds[0] < 0) {
		return false;
	}

	addr.sun_path[0] = '\0';
	length = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, SOCKET_DIR "/X%d", display);
	x11->fds[1] = listen_on(&addr, base + 1 + (socklen_t)length);
	if (x11->fds[1] < 0) {
		close(x11->fds[0]);
		snprintf(addr.sun_path, sizeof(addr.sun_path), SOCKET_DIR "/X%d", display);
		unlink(addr.sun_path);
		return false;
	}
	return true;
}

static void close_display(struct tw_x11 *x11) {
	char path[64];
	close(x11->fds[0]);
	close(x11->fds[1]);
	snprintf(path, sizeof(path), SOCKET_DIR "/X%d", x11->display);
	unlink(path);
	lock_path(path, sizeof(path), x11->display);
	unlink(path);
}

static bool open_display(struct tw_x11 *x11) {
	if (!socket_dir_ok()) {
		wlr_log(WLR_ERROR, "cannot use " SOCKET_DIR " for X11 sockets");
		return false;
	}
	for (int display = 0; display <= MAX_DISPLAY; display++) {
		if (!lock_display(display)) {
			continue;
		}
		if (open_sockets(x11, display)) {
			x11->display = display;
			snprintf(x11->display_name, sizeof(x11->display_name), ":%d", display);
			return true;
		}
		char path[32];
		lock_path(path, sizeof(path), display);
		unlink(path);
	}
	wlr_log(WLR_ERROR, "no free X11 display number");
	return false;
}

/* --- The helper ------------------------------------------------------------- */

static int handle_client(int fd, uint32_t mask, void *data);

static void unwatch(struct tw_x11 *x11) {
	for (size_t i = 0; i < LENGTH(x11->watch); i++) {
		if (x11->watch[i] != NULL) {
			wl_event_source_remove(x11->watch[i]);
			x11->watch[i] = NULL;
		}
	}
}

static void watch(struct tw_x11 *x11) {
	for (size_t i = 0; i < LENGTH(x11->watch); i++) {
		/* Whoever is still queued was waiting for a helper that died or
		 * never started. Turn them away: starting another helper for them
		 * would repeat for as long as it keeps failing. */
		struct pollfd pending = {.fd = x11->fds[i], .events = POLLIN};
		while (poll(&pending, 1, 0) > 0 && (pending.revents & POLLIN)) {
			int client = accept(x11->fds[i], NULL, NULL);
			if (client < 0) {
				break;
			}
			close(client);
		}
		x11->watch[i] = wl_event_loop_add_fd(x11->server->loop, x11->fds[i],
			WL_EVENT_READABLE, handle_client, x11);
	}
}

static int handle_helper_exit(int fd, uint32_t mask, void *data) {
	struct tw_x11 *x11 = data;
	int status = 0;
	while (waitpid(x11->pid, &status, 0) < 0 && errno == EINTR) {
	}
	/* X11 applications go down with it, so this is worth a line in the log. */
	if (WIFEXITED(status)) {
		wlr_log(WLR_ERROR, "%s exited with status %d", x11_helper, WEXITSTATUS(status));
	} else {
		wlr_log(WLR_ERROR, "%s was killed by signal %d", x11_helper, WTERMSIG(status));
	}
	wl_event_source_remove(x11->exit_source);
	x11->exit_source = NULL;
	close(x11->pidfd);
	x11->pid = 0;
	/* The next X11 application starts a new one. */
	watch(x11);
	return 0;
}

static bool helper_start(struct tw_x11 *x11) {
	/* Everything the child needs is prepared here. After fork() it may only
	 * make async-signal-safe calls: a driver thread could have held the
	 * allocator lock at the moment of the fork. */
	char fd_file[12], fd_abstract[12];
	snprintf(fd_file, sizeof(fd_file), "%d", x11->fds[0]);
	snprintf(fd_abstract, sizeof(fd_abstract), "%d", x11->fds[1]);
	char *argv[] = {x11->program, x11->display_name,
		"-listenfd", fd_file, "-listenfd", fd_abstract, NULL};

	/* Our environment minus DISPLAY: the helper is the display. */
	size_t count = 0;
	while (environ[count] != NULL) {
		count++;
	}
	char **envp = calloc(count + 1, sizeof(*envp));
	if (envp == NULL) {
		return false;
	}
	for (size_t i = 0, n = 0; i < count; i++) {
		if (strncmp(environ[i], "DISPLAY=", 8) != 0) {
			envp[n++] = environ[i];
		}
	}
	bool quiet = wlr_log_get_verbosity() < WLR_DEBUG;

	pid_t pid = fork();
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		/* The sockets are close-on-exec so that no other program inherits
		 * them. The helper is the one that must. */
		fcntl(x11->fds[0], F_SETFD, 0);
		fcntl(x11->fds[1], F_SETFD, 0);
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
			if (quiet) {
				dup2(null, STDERR_FILENO);
			}
			if (null > STDERR_FILENO) {
				close(null);
			}
		}
		execve(x11->program, argv, envp);
		_exit(127);
	}
	free(envp);
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed");
		return false;
	}

	int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
	struct wl_event_source *source = pidfd < 0 ? NULL : wl_event_loop_add_fd(
		x11->server->loop, pidfd, WL_EVENT_READABLE, handle_helper_exit, x11);
	if (source == NULL) {
		/* Without this we would never learn that it exited. */
		wlr_log(WLR_ERROR, "cannot watch %s, stopping it", x11_helper);
		if (pidfd >= 0) {
			close(pidfd);
		}
		kill(pid, SIGKILL);
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
		}
		return false;
	}
	x11->pid = pid;
	x11->pidfd = pidfd;
	x11->exit_source = source;
	wlr_log(WLR_DEBUG, "started %s for display %s", x11_helper, x11->display_name);
	return true;
}

static int handle_client(int fd, uint32_t mask, void *data) {
	struct tw_x11 *x11 = data;
	unwatch(x11);
	if (!helper_start(x11)) {
		watch(x11);
	}
	return 0;
}

/* --- Setup ------------------------------------------------------------------ */

void x11_init(struct tw_server *server) {
	/* Programs we start must never open windows on some other display: either
	 * DISPLAY names ours, or there is none. */
	unsetenv("DISPLAY");
	if (x11_helper[0] == '\0') {
		return;
	}
	char program[4096];
	if (!program_find(x11_helper, program, sizeof(program))) {
		wlr_log(WLR_INFO, "%s is not installed, X11 applications will not start", x11_helper);
		return;
	}
	struct tw_x11 *x11 = calloc(1, sizeof(*x11));
	if (x11 == NULL) {
		return;
	}
	x11->server = server;
	x11->program = strdup(program);
	if (x11->program == NULL || !open_display(x11)) {
		free(x11->program);
		free(x11);
		return;
	}
	watch(x11);
	setenv("DISPLAY", x11->display_name, true);
	server->x11 = x11;
	wlr_log(WLR_INFO, "X11 applications use DISPLAY=%s", x11->display_name);
}

void x11_finish(struct tw_server *server) {
	struct tw_x11 *x11 = server->x11;
	if (x11 == NULL) {
		return;
	}
	unwatch(x11);
	if (x11->pid != 0) {
		wl_event_source_remove(x11->exit_source);
		close(x11->pidfd);
		/* It would notice our socket closing anyway; this is just prompt. */
		kill(x11->pid, SIGTERM);
	}
	close_display(x11);
	free(x11->program);
	free(x11);
	server->x11 = NULL;
}
