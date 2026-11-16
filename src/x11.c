/*
 * X11 applications.
 *
 * tw speaks no X11. xwayland-satellite does: it runs Xwayland and shows each
 * X11 window to tw as an ordinary Wayland window. Both are started only when
 * the first X11 application connects, the way niri does it: tw owns the X11
 * display sockets, watches them, and hands them to the helper when a client
 * turns up.
 *
 * Cost while no X11 application runs: two listening sockets and two entries
 * in the event loop. No process, no wakeups. The helper and Xwayland run for
 * as long as an X11 application is connected, and tw stops them again when
 * the last one has gone: plenty of programs only ask X11 a question as they
 * start, and would otherwise leave an X server behind for the whole session.
 * tw is told of the helper's exit through a pidfd: one wakeup, no polling, no
 * SIGCHLD handler.
 */
#define _GNU_SOURCE /* syscall(), for pidfd_open */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
	/* Fires once, some time after the helper started or a window closed, to
	 * see whether any X11 application is left. NULL if the helper is never
	 * stopped. */
	struct wl_event_source *idle_timer;
	bool stopping; /* we asked the helper to exit */
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

/* turn_away: whoever is still queued was waiting for a helper that died or
 * never started. Starting another helper for them would repeat for as long
 * as it keeps failing, so they are refused. A client queued behind a helper
 * that we stopped ourselves is served by the next one instead. */
static void watch(struct tw_x11 *x11, bool turn_away) {
	for (size_t i = 0; i < LENGTH(x11->watch); i++) {
		struct pollfd pending = {.fd = x11->fds[i], .events = POLLIN};
		while (turn_away && poll(&pending, 1, 0) > 0 && (pending.revents & POLLIN)) {
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
	if (x11->stopping) {
		wlr_log(WLR_DEBUG, "%s stopped", x11_helper);
	} else if (WIFEXITED(status)) {
		/* X11 applications go down with it, so this is worth a line in the log. */
		wlr_log(WLR_ERROR, "%s exited with status %d", x11_helper, WEXITSTATUS(status));
	} else {
		wlr_log(WLR_ERROR, "%s was killed by signal %d", x11_helper, WTERMSIG(status));
	}
	wl_event_source_remove(x11->exit_source);
	x11->exit_source = NULL;
	close(x11->pidfd);
	x11->pid = 0;
	if (x11->idle_timer != NULL) {
		wl_event_source_timer_update(x11->idle_timer, 0);
	}
	/* The next X11 application starts a new one. */
	watch(x11, !x11->stopping);
	x11->stopping = false;
	return 0;
}

/* --- Stopping a helper nobody uses ---------------------------------------- */

/* The helper and Xwayland hold a handful of sockets when idle; an X11
 * application adds one. Far more than this means applications are connected. */
#define MAX_SOCKETS 48
#define MAX_CHILDREN 8

/* Adds the inodes of the sockets a process has open. False if they cannot
 * be read or do not fit. */
static bool sockets_of(pid_t pid, unsigned long *inodes, size_t *count) {
	char path[40], target[64];
	snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
	DIR *dir = opendir(path);
	if (dir == NULL) {
		return false;
	}
	bool ok = true;
	struct dirent *entry;
	while (ok && (entry = readdir(dir)) != NULL) {
		ssize_t length = readlinkat(dirfd(dir), entry->d_name, target, sizeof(target) - 1);
		if (length <= 0) {
			continue; /* "." and "..", or closed meanwhile */
		}
		target[length] = '\0';
		unsigned long inode;
		if (sscanf(target, "socket:[%lu]", &inode) == 1) {
			ok = *count < MAX_SOCKETS;
			if (ok) {
				inodes[(*count)++] = inode;
			}
		}
	}
	closedir(dir);
	return ok;
}

/* Lists the helper's child processes: Xwayland. False if that cannot be read
 * (the kernel may be built without the file). */
static bool children_of(pid_t pid, pid_t *children, size_t *count) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
	DIR *dir = opendir(path);
	if (dir == NULL) {
		return false;
	}
	bool ok = true;
	struct dirent *entry;
	while (ok && (entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		/* A child is listed under the thread that started it. */
		snprintf(path, sizeof(path), "/proc/%d/task/%.20s/children", (int)pid, entry->d_name);
		FILE *file = fopen(path, "re");
		if (file == NULL) {
			ok = false;
			continue;
		}
		int child;
		while (ok && fscanf(file, "%d", &child) == 1) {
			ok = *count < MAX_CHILDREN;
			if (ok) {
				children[(*count)++] = (pid_t)child;
			}
		}
		fclose(file);
	}
	closedir(dir);
	return ok;
}

/*
 * Whether an X11 application is connected to our display, or about to be.
 *
 * tw speaks no X11, so it cannot ask the server. The kernel knows, though: a
 * connection that came in through one of the display's sockets carries that
 * socket's name, and /proc/net/unix lists every Unix socket with its name.
 * So look at the sockets the helper and Xwayland have open. One that bears
 * the display's name is an application. One the list does not show at all
 * came from a sandbox with a network of its own, and is taken for one too.
 * The rest are theirs: the sockets they listen on, and the pairs they talk
 * to each other and to us through.
 *
 * Whenever something cannot be read the answer is yes, and the helper stays.
 */
static bool clients_connected(struct tw_x11 *x11) {
	unsigned long inodes[MAX_SOCKETS];
	bool ours[MAX_SOCKETS] = {false};
	pid_t children[MAX_CHILDREN];
	size_t count = 0, child_count = 0;
	if (!sockets_of(x11->pid, inodes, &count) ||
			!children_of(x11->pid, children, &child_count)) {
		return true;
	}
	for (size_t i = 0; i < child_count; i++) {
		if (!sockets_of(children[i], inodes, &count)) {
			return true;
		}
	}

	FILE *table = fopen("/proc/net/unix", "re");
	if (table == NULL) {
		return true;
	}
	char name[40];
	snprintf(name, sizeof(name), SOCKET_DIR "/X%d", x11->display);
	bool connected = false;
	char *line = NULL;
	size_t size = 0;
	while (!connected && getline(&line, &size, table) > 0) {
		/* "<address>: <references> <protocol> <flags> <type> <state> <inode>
		 * <name>", the name being absent for most, and starting with @ for
		 * an abstract socket. State 1 listens, 2 waits to be accepted, 3 is
		 * connected. */
		unsigned state;
		unsigned long inode;
		int offset = -1;
		if (sscanf(line, "%*s %*x %*x %*x %*x %x %lu %n", &state, &inode, &offset) < 2 ||
				offset < 0) {
			continue; /* the heading */
		}
		char *path = line + offset;
		path[strcspn(path, "\n")] = '\0';
		bool display = strcmp(path[0] == '@' ? path + 1 : path, name) == 0;
		if (display && state == 2) {
			connected = true; /* in the queue; no process holds it yet */
		}
		for (size_t i = 0; i < count; i++) {
			if (inodes[i] == inode) {
				ours[i] = true;
				connected = connected || (display && state != 1);
			}
		}
	}
	free(line);
	fclose(table);
	for (size_t i = 0; i < count; i++) {
		connected = connected || !ours[i];
	}
	return connected;
}

static int handle_idle_check(void *data) {
	struct tw_x11 *x11 = data;
	if (x11->pid != 0 && !x11->stopping && !clients_connected(x11)) {
		wlr_log(WLR_DEBUG, "no X11 application is connected, stopping %s", x11_helper);
		/* Xwayland goes with it; handle_helper_exit() sees to the rest. */
		x11->stopping = true;
		kill(x11->pid, SIGTERM);
	}
	return 0;
}

/* Applications come and go without tw hearing of it, and looking now and then
 * would be polling. So look once after the helper started, which catches a
 * program that only asked a question, and once after any window closed,
 * which is when an application tends to exit. One that leaves with no window
 * to close is noticed the next time a window does. */
static void check_later(struct tw_x11 *x11) {
	if (x11->idle_timer != NULL) {
		wl_event_source_timer_update(x11->idle_timer, x11->server->x11_idle_ms);
	}
}

void x11_window_closed(struct tw_server *server) {
	struct tw_x11 *x11 = server->x11;
	if (x11 != NULL && x11->pid != 0) {
		check_later(x11);
	}
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
		child_reset_signals();
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
	check_later(x11);
	wlr_log(WLR_DEBUG, "started %s for display %s", x11_helper, x11->display_name);
	return true;
}

static int handle_client(int fd, uint32_t mask, void *data) {
	struct tw_x11 *x11 = data;
	unwatch(x11);
	if (!helper_start(x11)) {
		watch(x11, true);
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
	char program[PATH_MAX];
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
	if (server->x11_idle_ms > 0) {
		/* Without the timer the helper just stays, as it would with 0. */
		x11->idle_timer = wl_event_loop_add_timer(server->loop, handle_idle_check, x11);
	}
	watch(x11, true);
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
	if (x11->idle_timer != NULL) {
		wl_event_source_remove(x11->idle_timer);
	}
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
