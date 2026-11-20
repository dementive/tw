/*
 * tw-control: what a bar and its menus would give, in one box. The time,
 * Wi-Fi, Bluetooth, volume and where it plays, the microphone, brightness,
 * the battery and the power mode, the monitors, and
 * locking, logging out, suspending, restarting and powering off.
 *
 * tw starts it for Mod+Escape; it is gone again on Escape, and the same key
 * closes it. While it is closed nothing of it runs.
 *
 *   tw-control      the box
 *   -d              say what it does, on stderr
 *
 * Keys: Up and Down pick a row, Left and Right change it, Return opens it or
 * does it (the Wi-Fi and Bluetooth rows open a list of networks or devices,
 * where Return connects and Escape comes back). Logging out, restarting and
 * powering off ask for Return a second time.
 *
 * Wi-Fi is iwd's and Bluetooth BlueZ's, asked over the system bus with no
 * library (apps/common/dbus.c); a network's password is typed here, when iwd
 * asks for it. Volume goes through wpctl, light through brightnessctl, the
 * power mode through tw-power and Bluetooth on and off through tw-bluetooth;
 * the battery is read from /sys.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "box.h"
#include "config.h"
#include "dbus.h"
#include "draw.h"

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))

static bool verbose;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...) {
	if (verbose) {
		va_list args;
		va_start(args, format);
		vfprintf(stderr, format, args);
		va_end(args);
		fputc('\n', stderr);
	}
}

static void copy(char *to, size_t size, const char *from) {
	snprintf(to, size, "%s", from != NULL ? from : "");
}

/* What the box says on its last line until the next key: a question, or how
 * something went. */
static char status[160];

static void tell(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void tell(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vsnprintf(status, sizeof(status), format, args);
	va_end(args);
	say("status: %s", status);
	box_redraw();
}

/* --- Programs ---------------------------------------------------------------- */

/* Runs a shell command and keeps what it prints, up to `size` bytes; true if
 * it ran and succeeded. For the quick ones: wpctl, brightnessctl. */
static bool command_read(const char *command, char *out, size_t size) {
	out[0] = '\0';
	FILE *pipe = popen(command, "re");
	if (pipe == NULL) {
		return false;
	}
	size_t got = fread(out, 1, size - 1, pipe);
	out[got] = '\0';
	return pclose(pipe) == 0;
}

static bool command_run(const char *format, ...) __attribute__((format(printf, 1, 2)));
static bool command_run(const char *format, ...) {
	char command[512], ignored[256];
	va_list args;
	va_start(args, format);
	vsnprintf(command, sizeof(command), format, args);
	va_end(args);
	say("run %s", command);
	return command_read(command, ignored, sizeof(ignored));
}

/* Whether a program is installed: on PATH, or a path. */
static bool installed(const char *command) {
	char program[256];
	snprintf(program, sizeof(program), "%.*s", (int)strcspn(command, " "), command);
	if (strchr(program, '/') != NULL) {
		return access(program, X_OK) == 0;
	}
	const char *dirs = getenv("PATH");
	char path[4096];
	while (dirs != NULL && *dirs != '\0') {
		size_t length = strcspn(dirs, ":");
		if (length > 0 && snprintf(path, sizeof(path), "%.*s/%s", (int)length, dirs, program) <
				(int)sizeof(path) && access(path, X_OK) == 0) {
			return true;
		}
		dirs += length + (dirs[length] == ':');
	}
	return false;
}

static void child_start(void) {
	signal(SIGPIPE, SIG_DFL);
	int null = open("/dev/null", O_RDONLY);
	if (null > STDIN_FILENO) {
		dup2(null, STDIN_FILENO);
		close(null);
	}
}

/* Starts a command that is to outlive this program: a monitors box, the
 * locker, a restart. */
static void command_leave(const char *command) {
	say("start %s", command);
	pid_t pid = fork();
	if (pid == 0) {
		child_start();
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "sh", "-c", command, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid > 0) {
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
		}
	}
}

/* Commands that take a while (Bluetooth on, a power profile): run beside
 * the box, which waits on their pidfd and calls `done` when they exit. */
static struct {
	pid_t pid;
	int pidfd;
	void (*done)(bool ok);
} jobs[4];

static void jobs_check(void) {
	for (size_t i = 0; i < LENGTH(jobs); i++) {
		int wstatus;
		if (jobs[i].pid <= 0 || waitpid(jobs[i].pid, &wstatus, WNOHANG) != jobs[i].pid) {
			continue;
		}
		box_watch(jobs[i].pidfd, NULL);
		close(jobs[i].pidfd);
		void (*done)(bool) = jobs[i].done;
		jobs[i].pid = 0;
		done(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
	}
}

static bool command_job(void (*done)(bool ok), const char *format, ...)
	__attribute__((format(printf, 2, 3)));
static bool command_job(void (*done)(bool ok), const char *format, ...) {
	char command[512];
	va_list args;
	va_start(args, format);
	vsnprintf(command, sizeof(command), format, args);
	va_end(args);
	size_t slot = 0;
	while (slot < LENGTH(jobs) && jobs[slot].pid > 0) {
		slot++;
	}
	if (slot == LENGTH(jobs)) {
		return false;
	}
	say("run %s", command);
	pid_t pid = fork();
	if (pid == 0) {
		child_start();
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}
	int pidfd = pid > 0 ? (int)syscall(SYS_pidfd_open, pid, 0) : -1;
	if (pidfd < 0) {
		if (pid > 0) {
			while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
			}
		}
		return false;
	}
	jobs[slot].pid = pid;
	jobs[slot].pidfd = pidfd;
	jobs[slot].done = done;
	box_watch(pidfd, jobs_check);
	return true;
}

/* --- Sound and light --------------------------------------------------------- */

static struct {
	bool there;   /* wpctl answers */
	int volume;   /* percent */
	bool muted, mic_there, mic_muted;
	struct {
		int id;
		char name[96];
		bool chosen;
	} outputs[8];
	size_t output_count;
} sound;

static struct {
	bool there;
	int percent;
} screen;

/* "Volume: 0.40 [MUTED]" */
static bool volume_parse(const char *text, int *percent, bool *muted) {
	double value;
	if (sscanf(text, "Volume: %lf", &value) != 1) {
		return false;
	}
	*percent = (int)(value * 100 + 0.5);
	*muted = strstr(text, "MUTED") != NULL;
	return true;
}

/*
 * The outputs as `wpctl status` lists them under Audio, Sinks:
 *   │  *   48. Built-in Audio Analog Stereo        [vol: 0.40]
 * The star is the one in use.
 */
static void outputs_parse(const char *text) {
	sound.output_count = 0;
	bool in_audio = false, in_sinks = false;
	for (const char *line = text; *line != '\0';) {
		const char *end = strchrnul(line, '\n');
		char copy_line[256];
		snprintf(copy_line, sizeof(copy_line), "%.*s", (int)(end - line), line);
		line = *end != '\0' ? end + 1 : end;
		if (strncmp(copy_line, "Audio", 5) == 0) {
			in_audio = true;
			continue;
		}
		if (copy_line[0] != ' ' && copy_line[0] != '\0' && strncmp(copy_line, "Audio", 5) != 0 &&
				(unsigned char)copy_line[0] < 0x80) {
			in_audio = false; /* Video, Settings */
		}
		if (!in_audio) {
			continue;
		}
		if (strstr(copy_line, "Sinks:") != NULL) {
			in_sinks = true;
			continue;
		}
		if (strstr(copy_line, ":") != NULL && strstr(copy_line, "[vol:") == NULL) {
			in_sinks = false; /* the next part: Sources, Filters */
		}
		if (!in_sinks || sound.output_count == LENGTH(sound.outputs)) {
			continue;
		}
		/* Past the tree's lines and the star to the number. */
		const char *at = copy_line;
		bool chosen = false;
		while (*at != '\0' && !(*at >= '0' && *at <= '9')) {
			chosen = chosen || *at == '*';
			at++;
		}
		int id, used = 0;
		if (sscanf(at, "%d. %n", &id, &used) != 1 || used == 0) {
			continue;
		}
		const char *name = at + used, *name_end = strstr(name, "[vol:");
		size_t length = name_end != NULL ? (size_t)(name_end - name) : strlen(name);
		while (length > 0 && name[length - 1] == ' ') {
			length--;
		}
		sound.outputs[sound.output_count].id = id;
		sound.outputs[sound.output_count].chosen = chosen;
		snprintf(sound.outputs[sound.output_count].name, sizeof(sound.outputs[0].name), "%.*s",
			(int)length, name);
		sound.output_count++;
	}
}

static void sound_read(void) {
	char text[8192];
	sound.there = command_read("wpctl get-volume " SPEAKERS " 2>/dev/null", text, sizeof(text)) &&
		volume_parse(text, &sound.volume, &sound.muted);
	int ignored;
	sound.mic_there = command_read("wpctl get-volume " MICROPHONE " 2>/dev/null", text,
		sizeof(text)) && volume_parse(text, &ignored, &sound.mic_muted);
	if (sound.there && command_read("wpctl status 2>/dev/null", text, sizeof(text))) {
		outputs_parse(text);
	} else {
		sound.output_count = 0;
	}
}

/* brightnessctl -m: "intel_backlight,backlight,400,40%,1000" */
static bool light_parse(const char *text, int *now, int *percent, int *most) {
	const char *field = text;
	int values[5] = {0}, i = 0;
	for (; i < 5 && field != NULL; i++) {
		values[i] = atoi(field);
		field = strchr(field, ',');
		field = field != NULL ? field + 1 : NULL;
	}
	if (i < 5 || text[0] == '\0') {
		return false;
	}
	*now = values[2];
	*percent = values[3];
	*most = values[4];
	return true;
}

static void light_read(void) {
	char text[512], command[256];
	int now, most;
	snprintf(command, sizeof(command), "brightnessctl -m %s info 2>/dev/null", screen_light);
	screen.there = command_read(command, text, sizeof(text)) &&
		light_parse(text, &now, &screen.percent, &most);
}

/* --- Battery and power mode -------------------------------------------------- */

static const char *sys_root(void) {
	const char *sys = getenv("TW_SYS"); /* the tests point it at a made-up machine */
	return sys != NULL ? sys : "/sys";
}

static bool file_read(const char *path, char *out, size_t size) {
	FILE *stream = fopen(path, "re");
	if (stream == NULL) {
		return false;
	}
	bool got = fgets(out, (int)size, stream) != NULL;
	fclose(stream);
	out[strcspn(out, "\n")] = '\0';
	return got;
}

static long supply_value(const char *dir, const char *name) {
	char path[4096], text[64];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return file_read(path, text, sizeof(text)) ? atol(text) : -1;
}

static char battery[96];  /* "74%, 3 h 10 min left"; "" without one */
static char profile[32];  /* "saver"; "" without tw-power */
static bool profile_busy;

static void battery_read(void) {
	battery[0] = '\0';
	char dir_path[4096];
	snprintf(dir_path, sizeof(dir_path), "%s/class/power_supply", sys_root());
	DIR *dir = opendir(dir_path);
	struct dirent *found;
	while (dir != NULL && (found = readdir(dir)) != NULL) {
		char supply[4400], path[4500], type[32], state[32] = "", scope[32] = "";
		snprintf(supply, sizeof(supply), "%s/%s", dir_path, found->d_name);
		snprintf(path, sizeof(path), "%s/type", supply);
		if (found->d_name[0] == '.' || !file_read(path, type, sizeof(type)) ||
				strcmp(type, "Battery") != 0) {
			continue;
		}
		/* A mouse's or a pen's battery is not the machine's. */
		snprintf(path, sizeof(path), "%s/scope", supply);
		if (file_read(path, scope, sizeof(scope)) && strcmp(scope, "Device") == 0) {
			continue;
		}
		long capacity = supply_value(supply, "capacity");
		snprintf(path, sizeof(path), "%s/status", supply);
		file_read(path, state, sizeof(state));
		/* Energy in µWh and power in µW, or charge in µAh and current in µA. */
		long now = supply_value(supply, "energy_now"), full = supply_value(supply, "energy_full");
		long rate = supply_value(supply, "power_now");
		if (now < 0 || rate < 0) {
			now = supply_value(supply, "charge_now");
			full = supply_value(supply, "charge_full");
			rate = supply_value(supply, "current_now");
		}
		long minutes = -1;
		if (rate > 0 && strcmp(state, "Discharging") == 0 && now >= 0) {
			minutes = now * 60 / rate;
		} else if (rate > 0 && strcmp(state, "Charging") == 0 && full > now && now >= 0) {
			minutes = (full - now) * 60 / rate;
		}
		int at = snprintf(battery, sizeof(battery), "%ld%%", capacity < 0 ? 0 : capacity);
		if (minutes >= 0) {
			snprintf(battery + at, sizeof(battery) - (size_t)at, ", %ld h %ld min %s",
				minutes / 60, minutes % 60, strcmp(state, "Charging") == 0 ? "to full" : "left");
		} else if (strcmp(state, "Charging") == 0 || strcmp(state, "Full") == 0 ||
				strcmp(state, "Not charging") == 0) {
			snprintf(battery + at, sizeof(battery) - (size_t)at, ", %s",
				strcmp(state, "Charging") == 0 ? "charging" : "on the charger");
		}
		break;
	}
	if (dir != NULL) {
		closedir(dir);
	}
}

/* tw-power says "Power profile: saver (platform: quiet, CPUs: power)". */
static void profile_read(void) {
	char text[256];
	profile[0] = '\0';
	if (!installed(profile_command) || !command_read(profile_command, text, sizeof(text))) {
		return;
	}
	const char *at = strstr(text, "profile: ");
	if (at != NULL) {
		at += 9;
		snprintf(profile, sizeof(profile), "%.*s", (int)strcspn(at, " \n"), at);
	}
}

static void profile_done(bool ok) {
	profile_busy = false;
	profile_read();
	if (!ok) {
		tell("The power mode could not be changed.");
	}
	box_redraw();
}

static void profile_change(int by) {
	if (profile_busy || profile[0] == '\0') {
		return;
	}
	size_t at = 0;
	for (size_t i = 0; i < LENGTH(profiles); i++) {
		at = strcmp(profiles[i], profile) == 0 ? i : at;
	}
	long next = (long)at + by;
	if (next < 0 || next >= (long)LENGTH(profiles)) {
		return;
	}
	if (command_job(profile_done, "%s %s", profile_command, profiles[next])) {
		profile_busy = true;
		copy(profile, sizeof(profile), profiles[next]);
	}
}

/* --- Wi-Fi: iwd -------------------------------------------------------------- */

#define IWD "net.connman.iwd"
#define AGENT_PATH "/tw/control"

struct network {
	char path[160];
	char name[64];
	char type[16];   /* "open", "psk", "8021x" */
	bool connected, known;
	int strength;    /* in hundredths of a dBm */
	bool in_range;   /* in the last scan */
};

static struct {
	bool there;           /* iwd answers */
	bool powered;
	char device[160], station[160];
	char state[32];       /* "connected", "connecting", "disconnected" */
	bool asking, again;   /* a look is under way, and another is wanted */
	bool agent;           /* our agent is registered */
	struct network networks[64];
	size_t network_count;
	size_t order[64];     /* in range, strongest first */
	size_t order_count;
	char connecting[64];  /* the network being connected to */
} wifi;

/* A password iwd is waiting for: the call to answer, and what is typed. */
static struct {
	bool asked;
	uint32_t serial;
	char sender[64];
	char network[64];
	char typed[128];
} secret;

struct object_look {
	const char *path, *interface;
	struct network *network;
};

static struct network *network_at(const char *path) {
	for (size_t i = 0; i < wifi.network_count; i++) {
		if (strcmp(wifi.networks[i].path, path) == 0) {
			return &wifi.networks[i];
		}
	}
	if (wifi.network_count == LENGTH(wifi.networks)) {
		return NULL;
	}
	struct network *network = &wifi.networks[wifi.network_count++];
	*network = (struct network){0};
	copy(network->path, sizeof(network->path), path);
	return network;
}

static bool read_bool(const char *type, struct bus_reader *value) {
	return strcmp(type, "b") == 0 && bus_read_u32(value) != 0;
}

static const char *read_text(const char *type, struct bus_reader *value) {
	return strcmp(type, "s") == 0 || strcmp(type, "o") == 0 ? bus_read_string(value) : "";
}

static void iwd_property(const char *name, const char *type, struct bus_reader *value,
		void *data) {
	struct object_look *look = data;
	if (strcmp(look->interface, IWD ".Device") == 0) {
		copy(wifi.device, sizeof(wifi.device), look->path);
		if (strcmp(name, "Powered") == 0) {
			wifi.powered = read_bool(type, value);
		}
	} else if (strcmp(look->interface, IWD ".Station") == 0) {
		copy(wifi.station, sizeof(wifi.station), look->path);
		if (strcmp(name, "State") == 0) {
			copy(wifi.state, sizeof(wifi.state), read_text(type, value));
		}
	} else if (strcmp(look->interface, IWD ".Network") == 0 && look->network != NULL) {
		if (strcmp(name, "Name") == 0) {
			copy(look->network->name, sizeof(look->network->name), read_text(type, value));
		} else if (strcmp(name, "Type") == 0) {
			copy(look->network->type, sizeof(look->network->type), read_text(type, value));
		} else if (strcmp(name, "Connected") == 0) {
			look->network->connected = read_bool(type, value);
		} else if (strcmp(name, "KnownNetwork") == 0) {
			look->network->known = true;
		}
	}
}

/* GetManagedObjects' answer: a{oa{sa{sv}}}, every object with every
 * interface it has and their properties. */
static void objects_read(struct bus_reader *reader,
		void (*property)(const char *, const char *, struct bus_reader *, void *),
		bool (*wanted)(struct object_look *look)) {
	size_t end = bus_read_array(reader, 8);
	while (!reader->failed && reader->pos < end) {
		bus_read_align(reader, 8);
		struct object_look look = {.path = bus_read_string(reader)};
		size_t interfaces_end = bus_read_array(reader, 8);
		while (!reader->failed && reader->pos < interfaces_end) {
			bus_read_align(reader, 8);
			look.interface = bus_read_string(reader);
			look.network = NULL;
			if (wanted(&look)) {
				dbus_properties(reader, property, &look);
			} else {
				bus_skip(reader, "a{sv}");
			}
		}
	}
}

static bool iwd_wanted(struct object_look *look) {
	if (strcmp(look->interface, IWD ".Network") == 0) {
		look->network = network_at(look->path);
	}
	return strncmp(look->interface, IWD ".", sizeof(IWD)) == 0;
}

static void iwd_refresh(void);

static void iwd_finished(void) {
	wifi.asking = false;
	if (wifi.again) {
		wifi.again = false;
		iwd_refresh();
	}
	box_redraw();
}

/* GetOrderedNetworks' answer: a(on), the networks in range, strongest first. */
static void iwd_ordered(const struct bus_message *reply, void *data) {
	wifi.order_count = 0;
	if (reply != NULL && reply->type == BUS_RETURN) {
		struct bus_reader reader = reply->body;
		size_t end = bus_read_array(&reader, 8);
		while (!reader.failed && reader.pos < end && wifi.order_count < LENGTH(wifi.order)) {
			bus_read_align(&reader, 8);
			const char *path = bus_read_string(&reader);
			int strength = bus_read_i16(&reader);
			for (size_t i = 0; i < wifi.network_count && !reader.failed; i++) {
				if (strcmp(wifi.networks[i].path, path) == 0) {
					wifi.networks[i].strength = strength;
					wifi.networks[i].in_range = true;
					wifi.order[wifi.order_count++] = i;
				}
			}
		}
	}
	iwd_finished();
}

static void iwd_objects(const struct bus_message *reply, void *data) {
	bool had = wifi.there;
	wifi.there = reply != NULL && reply->type == BUS_RETURN;
	wifi.powered = false;
	wifi.device[0] = wifi.station[0] = wifi.state[0] = '\0';
	wifi.network_count = 0;
	if (wifi.there) {
		struct bus_reader reader = reply->body;
		objects_read(&reader, iwd_property, iwd_wanted);
	}
	if (had != wifi.there) {
		say("wifi %s", wifi.there ? "there" : "not there");
	}
	if (wifi.station[0] != '\0') {
		dbus_call(IWD, wifi.station, IWD ".Station", "GetOrderedNetworks", NULL);
		dbus_send(iwd_ordered, NULL);
	} else {
		wifi.order_count = 0;
		iwd_finished();
	}
}

/* Asks iwd how everything is: one look at a time, and one more if anything
 * changed meanwhile. */
static void iwd_refresh(void) {
	if (dbus_fd() < 0) {
		return;
	}
	if (wifi.asking) {
		wifi.again = true;
		return;
	}
	wifi.asking = true;
	dbus_call(IWD, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL);
	dbus_send(iwd_objects, NULL);
}

static const struct network *wifi_connected(void) {
	for (size_t i = 0; i < wifi.network_count; i++) {
		if (wifi.networks[i].connected) {
			return &wifi.networks[i];
		}
	}
	return NULL;
}

/* Sets a boolean property (Powered, Trusted) through org.freedesktop.DBus.Properties. */
static void property_set(const char *destination, const char *path, const char *interface,
		const char *name, bool value, dbus_reply reply) {
	struct bus_writer *writer = dbus_call(destination, path, "org.freedesktop.DBus.Properties",
		"Set", "ssv");
	bus_write_string(writer, interface);
	bus_write_string(writer, name);
	bus_write_signature(writer, "b");
	bus_write_u32(writer, value);
	dbus_send(reply, NULL);
}

static void wifi_power(bool on) {
	if (wifi.device[0] != '\0') {
		property_set(IWD, wifi.device, IWD ".Device", "Powered", on, NULL);
	}
}

static void wifi_scan(void) {
	if (wifi.station[0] != '\0') {
		dbus_call(IWD, wifi.station, IWD ".Station", "Scan", NULL);
		dbus_send(NULL, NULL);
	}
}

/* So that iwd asks us for a network's password. */
static void wifi_agent(void) {
	if (!wifi.there || wifi.agent) {
		return;
	}
	wifi.agent = true;
	struct bus_writer *writer = dbus_call(IWD, "/net/connman/iwd", IWD ".AgentManager",
		"RegisterAgent", "o");
	bus_write_string(writer, AGENT_PATH);
	dbus_send(NULL, NULL);
}

/* The answer to Network.Connect: done, or why not. */
static void wifi_connected_reply(const struct bus_message *reply, void *data) {
	if (reply == NULL) {
		return;
	}
	if (reply->type == BUS_ERROR) {
		/* Its text says why, if it has one. */
		struct bus_reader body = reply->body;
		say("connect failed: %s", reply->signature[0] == 's' ? bus_read_string(&body) : "");
		tell("Could not connect to %s.", wifi.connecting);
	} else {
		tell("Connected to %s.", wifi.connecting);
	}
	wifi.connecting[0] = '\0';
	iwd_refresh();
}

static void wifi_pick(const struct network *network) {
	if (network->connected) {
		dbus_call(IWD, wifi.station, IWD ".Station", "Disconnect", NULL);
		dbus_send(NULL, NULL);
		tell("Disconnected from %s.", network->name);
		return;
	}
	wifi_agent();
	copy(wifi.connecting, sizeof(wifi.connecting), network->name);
	dbus_call(IWD, network->path, IWD ".Network", "Connect", NULL);
	dbus_send(wifi_connected_reply, NULL);
	tell("Connecting to %s…", network->name);
}

/* Answers iwd's RequestPassphrase with what was typed, or says no. */
static void secret_answer(bool give) {
	if (!secret.asked) {
		return;
	}
	struct bus_message call = {.serial = secret.serial, .sender = secret.sender};
	if (give) {
		bus_write_string(dbus_answer(&call, "s"), secret.typed);
		dbus_send(NULL, NULL);
	} else {
		dbus_answer_error(&call, IWD ".Agent.Error.Canceled");
	}
	explicit_bzero(secret.typed, sizeof(secret.typed));
	secret.asked = false;
	status[0] = '\0';
	box_redraw();
}

/* --- Bluetooth: BlueZ -------------------------------------------------------- */

#define BLUEZ "org.bluez"
#define BLUEZ_AGENT_PATH "/tw/control/bluetooth"

struct device {
	char path[160];
	char name[64];
	bool named;      /* it gave a name, not only its address */
	bool paired, connected;
	int rssi;
	bool near;       /* seen while looking */
};

static struct {
	bool driver;     /* an adapter is there (/sys/class/bluetooth) */
	bool there;      /* BlueZ answers */
	bool powered, discovering;
	char adapter[160];
	bool asking, again, agent, looking;
	bool switching;  /* tw-bluetooth is turning it on or off */
	struct device devices[64];
	size_t device_count;
	size_t order[64];
	size_t order_count;
} bt;

static bool bluetooth_driver(void) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/class/bluetooth", sys_root());
	DIR *dir = opendir(path);
	bool found = false;
	struct dirent *entry;
	while (dir != NULL && (entry = readdir(dir)) != NULL) {
		found = found || strncmp(entry->d_name, "hci", 3) == 0;
	}
	if (dir != NULL) {
		closedir(dir);
	}
	return found;
}

static struct device *device_at(const char *path) {
	for (size_t i = 0; i < bt.device_count; i++) {
		if (strcmp(bt.devices[i].path, path) == 0) {
			return &bt.devices[i];
		}
	}
	if (bt.device_count == LENGTH(bt.devices)) {
		return NULL;
	}
	struct device *device = &bt.devices[bt.device_count++];
	*device = (struct device){0};
	copy(device->path, sizeof(device->path), path);
	return device;
}

static void bluez_property(const char *name, const char *type, struct bus_reader *value,
		void *data) {
	struct object_look *look = data;
	if (strcmp(look->interface, BLUEZ ".Adapter1") == 0) {
		if (bt.adapter[0] == '\0') {
			copy(bt.adapter, sizeof(bt.adapter), look->path);
		}
		if (strcmp(name, "Powered") == 0) {
			bt.powered = read_bool(type, value);
		} else if (strcmp(name, "Discovering") == 0) {
			bt.discovering = read_bool(type, value);
		}
		return;
	}
	struct device *device = device_at(look->path);
	if (device == NULL) {
		return;
	}
	if (strcmp(name, "Name") == 0) {
		copy(device->name, sizeof(device->name), read_text(type, value));
		device->named = true;
	} else if (strcmp(name, "Alias") == 0 && !device->named) {
		copy(device->name, sizeof(device->name), read_text(type, value));
	} else if (strcmp(name, "Paired") == 0) {
		device->paired = read_bool(type, value);
	} else if (strcmp(name, "Connected") == 0) {
		device->connected = read_bool(type, value);
	} else if (strcmp(name, "RSSI") == 0 && strcmp(type, "n") == 0) {
		device->rssi = bus_read_i16(value);
		device->near = true;
	}
}

static bool bluez_wanted(struct object_look *look) {
	return strcmp(look->interface, BLUEZ ".Adapter1") == 0 ||
		strcmp(look->interface, BLUEZ ".Device1") == 0;
}

/* Connected first, then paired, then the nearest of the rest; a device that
 * gives no name and is not paired is left out. */
static int device_rank(const struct device *device) {
	return device->connected ? 0 : device->paired ? 1 : 2;
}

static int device_order(const void *a, const void *b) {
	const struct device *x = &bt.devices[*(const size_t *)a];
	const struct device *y = &bt.devices[*(const size_t *)b];
	if (device_rank(x) != device_rank(y)) {
		return device_rank(x) - device_rank(y);
	}
	if (x->rssi != y->rssi) {
		return y->rssi - x->rssi;
	}
	return strcasecmp(x->name, y->name);
}

static void bluez_refresh(void);

static void bluez_objects(const struct bus_message *reply, void *data) {
	bt.there = reply != NULL && reply->type == BUS_RETURN;
	bt.powered = bt.discovering = false;
	bt.adapter[0] = '\0';
	bt.device_count = 0;
	if (bt.there) {
		struct bus_reader reader = reply->body;
		objects_read(&reader, bluez_property, bluez_wanted);
	}
	bt.order_count = 0;
	for (size_t i = 0; i < bt.device_count; i++) {
		if (bt.devices[i].named || bt.devices[i].paired) {
			bt.order[bt.order_count++] = i;
		}
	}
	qsort(bt.order, bt.order_count, sizeof(bt.order[0]), device_order);
	bt.asking = false;
	if (bt.again) {
		bt.again = false;
		bluez_refresh();
	}
	box_redraw();
}

static void bluez_refresh(void) {
	bt.driver = bluetooth_driver();
	/* With the driver gone BlueZ is not asked: the call would start it. */
	if (!bt.driver || dbus_fd() < 0) {
		bt.there = bt.powered = false;
		bt.device_count = bt.order_count = 0;
		return;
	}
	if (bt.asking) {
		bt.again = true;
		return;
	}
	bt.asking = true;
	dbus_call(BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL);
	dbus_send(bluez_objects, NULL);
}

/* While the device list is open: our agent, so that pairing needs no PIN
 * typed, and looking for devices. BlueZ stops looking when we go. */
static void bluetooth_look(bool look) {
	if (!bt.there || !bt.powered || bt.adapter[0] == '\0' || look == bt.looking) {
		return;
	}
	if (look && !bt.agent) {
		bt.agent = true;
		struct bus_writer *writer = dbus_call(BLUEZ, "/org/bluez", BLUEZ ".AgentManager1",
			"RegisterAgent", "os");
		bus_write_string(writer, BLUEZ_AGENT_PATH);
		bus_write_string(writer, "NoInputNoOutput");
		dbus_send(NULL, NULL);
	}
	bt.looking = look;
	dbus_call(BLUEZ, bt.adapter, BLUEZ ".Adapter1", look ? "StartDiscovery" : "StopDiscovery",
		NULL);
	dbus_send(NULL, NULL);
}

static void bluetooth_switched(bool ok) {
	bt.switching = false;
	if (!ok) {
		tell("Bluetooth could not be switched.");
	}
	bluez_refresh();
	box_redraw();
}

static void bluetooth_switch(bool on) {
	if (bt.switching) {
		return;
	}
	if (installed(bluetooth_command)) {
		if (command_job(bluetooth_switched, "%s %s", bluetooth_command, on ? "on" : "off")) {
			bt.switching = true;
			tell(on ? "Turning Bluetooth on…" : "Turning Bluetooth off…");
		}
	} else if (bt.adapter[0] != '\0') {
		property_set(BLUEZ, bt.adapter, BLUEZ ".Adapter1", "Powered", on, NULL);
	}
}

/* Connecting, pairing first if it is new; the device's path is the data. */
static char bt_target[160], bt_target_name[64];

static void bluetooth_connected(const struct bus_message *reply, void *data) {
	if (reply != NULL) {
		if (reply->type == BUS_ERROR) {
			tell("Could not connect to %s.", bt_target_name);
		} else {
			tell("Connected to %s.", bt_target_name);
		}
	}
	bluez_refresh();
}

static void bluetooth_paired(const struct bus_message *reply, void *data) {
	if (reply == NULL) {
		return;
	}
	if (reply->type == BUS_ERROR) {
		tell("Could not pair with %s.", bt_target_name);
		return;
	}
	/* Trusted, so that it may connect again by itself. */
	property_set(BLUEZ, bt_target, BLUEZ ".Device1", "Trusted", true, NULL);
	dbus_call(BLUEZ, bt_target, BLUEZ ".Device1", "Connect", NULL);
	dbus_send(bluetooth_connected, NULL);
}

static void bluetooth_pick(const struct device *device) {
	copy(bt_target, sizeof(bt_target), device->path);
	copy(bt_target_name, sizeof(bt_target_name), device->name);
	if (device->connected) {
		dbus_call(BLUEZ, device->path, BLUEZ ".Device1", "Disconnect", NULL);
		dbus_send(NULL, NULL);
		tell("Disconnected from %s.", device->name);
	} else if (device->paired) {
		dbus_call(BLUEZ, device->path, BLUEZ ".Device1", "Connect", NULL);
		dbus_send(bluetooth_connected, NULL);
		tell("Connecting to %s…", device->name);
	} else {
		dbus_call(BLUEZ, device->path, BLUEZ ".Device1", "Pair", NULL);
		dbus_send(bluetooth_paired, NULL);
		tell("Pairing with %s…", device->name);
	}
}

/* --- The bus ----------------------------------------------------------------- */

/* Calls to our agents, and the signals of iwd and BlueZ. */
static void incoming(const struct bus_message *message) {
	if (message->type == BUS_SIGNAL) {
		if (strcmp(message->interface, "org.freedesktop.DBus.Properties") == 0 ||
				strcmp(message->interface, "org.freedesktop.DBus.ObjectManager") == 0) {
			if (strncmp(message->path, "/net/connman/iwd", 16) == 0 ||
					strcmp(message->path, "/") == 0) {
				iwd_refresh();
			}
			if (strncmp(message->path, "/org/bluez", 10) == 0 || strcmp(message->path, "/") == 0) {
				bluez_refresh();
			}
		}
		return;
	}
	if (message->type != BUS_CALL) {
		return;
	}
	if (strcmp(message->path, AGENT_PATH) == 0 && strcmp(message->member, "RequestPassphrase") == 0) {
		/* iwd wants the password of the network being connected to. */
		if (secret.asked) {
			secret_answer(false);
		}
		secret.asked = true;
		secret.serial = message->serial;
		copy(secret.sender, sizeof(secret.sender), message->sender);
		copy(secret.network, sizeof(secret.network), wifi.connecting);
		secret.typed[0] = '\0';
		say("password asked");
		box_redraw();
		return;
	}
	if (strcmp(message->path, AGENT_PATH) == 0 && strcmp(message->member, "Cancel") == 0) {
		secret.asked = false;
		explicit_bzero(secret.typed, sizeof(secret.typed));
		status[0] = '\0';
		box_redraw();
	}
	if (strcmp(message->path, BLUEZ_AGENT_PATH) == 0 &&
			(strcmp(message->member, "RequestPinCode") == 0 ||
			strcmp(message->member, "RequestPasskey") == 0)) {
		dbus_answer_error(message, BLUEZ ".Error.Rejected");
		return;
	}
	/* Release, Cancel, and BlueZ's questions that need no typing: yes. */
	if (strcmp(message->path, AGENT_PATH) == 0 || strcmp(message->path, BLUEZ_AGENT_PATH) == 0) {
		dbus_answer(message, NULL);
		dbus_send(NULL, NULL);
		return;
	}
	dbus_answer_error(message, "org.freedesktop.DBus.Error.UnknownMethod");
}

static void bus_ready(void) {
	if (!dbus_read()) {
		box_watch(dbus_fd(), NULL);
		wifi.there = bt.there = false;
		box_redraw();
	}
}

/* --- The pages --------------------------------------------------------------- */

enum page { PAGE_MAIN, PAGE_WIFI, PAGE_BLUETOOTH };
static enum page page;
static size_t cursor, first; /* the row picked on this page, the one at the top */

enum item {
	ITEM_WIFI, ITEM_BLUETOOTH, ITEM_VOLUME, ITEM_OUTPUT, ITEM_MIC, ITEM_BRIGHTNESS,
	ITEM_BATTERY, ITEM_PROFILE, ITEM_MONITORS, ITEM_SESSION, ITEMS
};
static const char *const item_names[ITEMS] = {
	"Wi-Fi", "Bluetooth", "Volume", "Output", "Microphone", "Brightness",
	"Battery", "Power mode", "Monitors", "Session",
};

enum session { SESSION_LOCK, SESSION_LOG_OUT, SESSION_SUSPEND, SESSION_RESTART, SESSION_POWER_OFF };
static const char *const session_names[] = {"Lock", "Log out", "Suspend", "Restart", "Power off"};
static enum session session = SESSION_LOCK;
static bool confirming; /* Return was pressed once on a session action */

static bool item_shown(enum item item) {
	switch (item) {
	case ITEM_BLUETOOTH:
		return bt.driver || installed(bluetooth_command);
	case ITEM_VOLUME:
		return sound.there;
	case ITEM_OUTPUT:
		return sound.output_count > 1;
	case ITEM_MIC:
		return sound.mic_there;
	case ITEM_BRIGHTNESS:
		return screen.there;
	case ITEM_BATTERY:
		return battery[0] != '\0';
	case ITEM_PROFILE:
		return profile[0] != '\0';
	case ITEM_MONITORS:
		return installed(monitors_command);
	default:
		return true;
	}
}

/* The main page's items in order, the hidden ones left out. */
static size_t items_shown(enum item *items) {
	size_t count = 0;
	for (enum item item = 0; item < ITEMS; item++) {
		if (item_shown(item)) {
			items[count++] = item;
		}
	}
	return count;
}

static size_t page_length(void) {
	enum item items[ITEMS];
	switch (page) {
	case PAGE_WIFI:
		return 1 + (wifi.powered ? wifi.order_count : 0);
	case PAGE_BLUETOOTH:
		return 1 + (bt.powered ? bt.order_count : 0);
	default:
		return items_shown(items);
	}
}

static const char *bars(int strength) {
	/* In hundredths of a dBm: -50 dBm is a strong signal, -80 a weak one. */
	return strength > -6000 ? "▂▄▆█" : strength > -6700 ? "▂▄▆" : strength > -7500 ? "▂▄" : "▂";
}

/* What a row says: its label, its value, whether Left and Right change it,
 * and a note in the hint colour after the value. */
struct line {
	char label[64], value[160], note[64];
	bool changes;
};

static void item_line(enum item item, struct line *line) {
	copy(line->label, sizeof(line->label), item_names[item]);
	const struct network *network;
	switch (item) {
	case ITEM_WIFI:
		line->changes = wifi.there && wifi.device[0] != '\0';
		network = wifi_connected();
		copy(line->value, sizeof(line->value), !wifi.there ? "not available" :
			!wifi.powered ? "off" : network != NULL ? network->name :
			strcmp(wifi.state, "connecting") == 0 ? "connecting…" : "not connected");
		break;
	case ITEM_BLUETOOTH: {
		line->changes = true;
		size_t connected = 0;
		const char *one = NULL;
		for (size_t i = 0; i < bt.device_count; i++) {
			if (bt.devices[i].connected) {
				connected++;
				one = bt.devices[i].name;
			}
		}
		if (bt.switching) {
			copy(line->value, sizeof(line->value), "switching…");
		} else if (!bt.driver || !bt.powered) {
			copy(line->value, sizeof(line->value), "off");
		} else if (connected == 1) {
			snprintf(line->value, sizeof(line->value), "on, %s", one);
		} else if (connected > 1) {
			snprintf(line->value, sizeof(line->value), "on, %zu connected", connected);
		} else {
			copy(line->value, sizeof(line->value), "on");
		}
		break;
	}
	case ITEM_VOLUME:
		line->changes = true;
		snprintf(line->value, sizeof(line->value), "%d%%%s", sound.volume,
			sound.muted ? ", muted" : "");
		break;
	case ITEM_OUTPUT:
		line->changes = true;
		for (size_t i = 0; i < sound.output_count; i++) {
			if (sound.outputs[i].chosen) {
				copy(line->value, sizeof(line->value), sound.outputs[i].name);
			}
		}
		break;
	case ITEM_MIC:
		line->changes = true;
		copy(line->value, sizeof(line->value), sound.mic_muted ? "muted" : "on");
		break;
	case ITEM_BRIGHTNESS:
		line->changes = true;
		snprintf(line->value, sizeof(line->value), "%d%%", screen.percent);
		break;
	case ITEM_BATTERY:
		copy(line->value, sizeof(line->value), battery);
		break;
	case ITEM_PROFILE:
		line->changes = true;
		copy(line->value, sizeof(line->value), profile);
		break;
	case ITEM_MONITORS:
		copy(line->value, sizeof(line->value), "open");
		break;
	case ITEM_SESSION:
		line->changes = true;
		copy(line->value, sizeof(line->value), session_names[session]);
		break;
	default:
		break;
	}
}

/* Row `row` of the page now shown. */
static void page_line(size_t row, struct line *line) {
	*line = (struct line){0};
	if (page == PAGE_MAIN) {
		enum item items[ITEMS];
		size_t count = items_shown(items);
		if (row < count) {
			item_line(items[row], line);
		}
		return;
	}
	if (page == PAGE_WIFI) {
		if (row == 0) {
			copy(line->label, sizeof(line->label), "Wi-Fi");
			copy(line->value, sizeof(line->value), wifi.powered ? "on" : "off");
			line->changes = true;
			return;
		}
		const struct network *network = &wifi.networks[wifi.order[row - 1]];
		copy(line->label, sizeof(line->label), bars(network->strength));
		copy(line->value, sizeof(line->value), network->name);
		copy(line->note, sizeof(line->note), network->connected ? "connected" :
			network->known ? "saved" : strcmp(network->type, "open") == 0 ? "open" : "secured");
		return;
	}
	if (row == 0) {
		copy(line->label, sizeof(line->label), "Bluetooth");
		copy(line->value, sizeof(line->value), bt.switching ? "switching…" :
			bt.powered ? "on" : "off");
		line->changes = true;
		return;
	}
	const struct device *device = &bt.devices[bt.order[row - 1]];
	copy(line->value, sizeof(line->value), device->name);
	copy(line->note, sizeof(line->note), device->connected ? "connected" :
		device->paired ? "paired" : "");
}

/* --- Drawing ----------------------------------------------------------------- */

enum { box_height = 2 * border + 2 * padding + row_height * (rows + 2) };

static int text_line(struct picture *picture, int x, int right, int baseline,
		const char *text, uint32_t colour) {
	if (x >= right) {
		return x;
	}
	int width = (right - x) * 64;
	size_t length = text_fit(text, width, false);
	bool cut = text[length] != '\0';
	if (cut) {
		length = text_fit(text, width - text_advance(0x2026), false);
	}
	return text_draw(picture, x, baseline, text, length, cut, colour);
}

/* What each row said when last drawn, so that -d says only what changed. */
static char said[rows][320];

static void draw(struct picture *picture) {
	font_size_set(box_px(font_size));
	int edge = box_px(border), inside = edge + box_px(padding), line_height = box_px(row_height);
	int right = picture->width - inside;
	picture_fill(picture, 0, 0, picture->width, picture->height, colour_border);
	picture_fill(picture, edge, edge, picture->width - 2 * edge, picture->height - 2 * edge,
		colour_background);
	int lift = (line_height - font_line_height()) / 2 + font_ascent();

	/* The top line: the time, or what the page is about. */
	char title[96];
	if (page == PAGE_MAIN) {
		time_t now = time(NULL);
		strftime(title, sizeof(title), clock_format, localtime(&now));
	} else {
		copy(title, sizeof(title), page == PAGE_WIFI ? "Wi-Fi networks" : "Bluetooth devices");
	}
	text_line(picture, inside, right, inside + lift, title, colour_selected_text);

	size_t length = page_length();
	if (cursor >= length) {
		cursor = length > 0 ? length - 1 : 0;
	}
	if (cursor < first) {
		first = cursor;
	} else if (cursor >= first + rows) {
		first = cursor - rows + 1;
	}
	int top = inside + line_height;
	for (size_t row = first, shown = 0; row < length && shown < rows; row++, shown++) {
		struct line line;
		page_line(row, &line);
		bool here = row == cursor;
		if (here) {
			picture_fill(picture, edge, top, picture->width - 2 * edge, line_height, colour_selected);
		}
		text_line(picture, inside, inside + box_px(label_width), top + lift, line.label,
			colour_label);
		char value[200];
		snprintf(value, sizeof(value), here && line.changes ? "‹ %s ›" : "%s", line.value);
		int end = text_line(picture, inside + box_px(label_width), right, top + lift, value,
			here ? colour_selected_text : colour_text);
		if (line.note[0] != '\0') {
			text_line(picture, end + box_px(10), right, top + lift, line.note, colour_hint);
		}
		top += line_height;
		if (verbose) {
			char text[320];
			snprintf(text, sizeof(text), "%s: %s%s%s", line.label, line.value,
				line.note[0] != '\0' ? " " : "", line.note);
			if (strcmp(said[shown], text) != 0) {
				copy(said[shown], sizeof(said[shown]), text);
				say("row %s", text);
			}
		}
	}

	/* The last line: a question, how something went, or what the keys do. */
	char hint[256];
	if (secret.asked) {
		char dots[128] = "";
		size_t count = 0;
		for (const char *c = secret.typed; *c != '\0' && count < 40; c++) {
			if ((*c & 0xc0) != 0x80) {
				strcat(dots, "•");
				count++;
			}
		}
		snprintf(hint, sizeof(hint), "Password for %s: %s", secret.network, dots);
	} else if (status[0] != '\0') {
		copy(hint, sizeof(hint), status);
	} else {
		copy(hint, sizeof(hint), page == PAGE_MAIN ?
			"Left and Right change, Return opens, Escape leaves" :
			"Return connects or disconnects, Escape goes back");
	}
	text_line(picture, inside, right, picture->height - inside - line_height + lift, hint,
		secret.asked ? colour_selected_text : colour_hint);
}

/* The clock moves on at the start of each minute. */
static void clock_timer(void) {
	time_t now = time(NULL);
	box_timer((int)(60 - now % 60) * 1000 + 50);
}

static void tick(void) {
	battery_read();
	clock_timer();
	box_redraw();
}

/* --- Doing ------------------------------------------------------------------- */

static void session_do(void) {
	box_quit();
	switch (session) {
	case SESSION_LOCK:
		command_leave(lock_command);
		break;
	case SESSION_SUSPEND:
		command_leave(suspend_command);
		break;
	case SESSION_RESTART:
		command_leave(restart_command);
		break;
	case SESSION_POWER_OFF:
		command_leave(power_off_command);
		break;
	case SESSION_LOG_OUT: {
		/* tw ends the session when told to stop, as Mod+Shift+E does. */
		pid_t compositor = box_compositor_pid();
		say("log out: stopping %d", (int)compositor);
		if (compositor > 0) {
			kill(compositor, SIGTERM);
		}
		break;
	}
	}
}

static void item_change(enum item item, int by) {
	char command[256];
	switch (item) {
	case ITEM_WIFI:
		wifi_power(by > 0);
		break;
	case ITEM_BLUETOOTH:
		bluetooth_switch(by > 0);
		break;
	case ITEM_VOLUME:
		command_run("wpctl set-volume -l 1.0 " SPEAKERS " %d%%%c", volume_step, by > 0 ? '+' : '-');
		sound_read();
		break;
	case ITEM_OUTPUT: {
		size_t at = 0;
		for (size_t i = 0; i < sound.output_count; i++) {
			at = sound.outputs[i].chosen ? i : at;
		}
		at = (at + sound.output_count + (by > 0 ? 1 : sound.output_count - 1)) % sound.output_count;
		command_run("wpctl set-default %d", sound.outputs[at].id);
		sound_read();
		break;
	}
	case ITEM_MIC:
		command_run("wpctl set-mute " MICROPHONE " toggle");
		sound_read();
		break;
	case ITEM_BRIGHTNESS:
		if (by < 0 && screen.percent - brightness_step < brightness_least) {
			snprintf(command, sizeof(command), "%d%%", brightness_least);
		} else {
			snprintf(command, sizeof(command), "%d%%%c", brightness_step, by > 0 ? '+' : '-');
		}
		command_run("brightnessctl -q %s set %s", screen_light, command);
		light_read();
		break;
	case ITEM_PROFILE:
		profile_change(by);
		break;
	case ITEM_SESSION:
		session = (enum session)(((int)session + (int)LENGTH(session_names) + by) %
			(int)LENGTH(session_names));
		break;
	default:
		break;
	}
}

static void page_open(enum page to) {
	if (page == PAGE_BLUETOOTH && to != PAGE_BLUETOOTH) {
		bluetooth_look(false);
	}
	page = to;
	cursor = first = 0;
	status[0] = '\0';
	if (to == PAGE_WIFI) {
		wifi_agent();
		wifi_scan();
		iwd_refresh();
	} else if (to == PAGE_BLUETOOTH) {
		bluez_refresh();
		bluetooth_look(true);
	}
}

static void item_open(enum item item) {
	switch (item) {
	case ITEM_WIFI:
		if (wifi.there) {
			page_open(PAGE_WIFI);
		}
		break;
	case ITEM_BLUETOOTH:
		if (!bt.driver || !bt.powered) {
			bluetooth_switch(true);
		} else {
			page_open(PAGE_BLUETOOTH);
		}
		break;
	case ITEM_VOLUME:
		command_run("wpctl set-mute " SPEAKERS " toggle");
		sound_read();
		break;
	case ITEM_MIC:
		item_change(item, 1);
		break;
	case ITEM_MONITORS:
		command_leave(monitors_command);
		box_quit();
		break;
	case ITEM_SESSION:
		if (session == SESSION_LOCK || session == SESSION_SUSPEND || confirming) {
			session_do();
		} else {
			confirming = true;
			tell("Return again to %s, Escape not to.", session == SESSION_LOG_OUT ? "log out" :
				session == SESSION_RESTART ? "restart" : "power off");
		}
		break;
	default:
		break;
	}
}

static void row_change(int by) {
	if (page == PAGE_MAIN) {
		enum item items[ITEMS];
		if (cursor < items_shown(items)) {
			item_change(items[cursor], by);
		}
	} else if (cursor == 0) {
		if (page == PAGE_WIFI) {
			wifi_power(by > 0);
		} else {
			bluetooth_switch(by > 0);
		}
	}
}

static void row_open(void) {
	if (page == PAGE_MAIN) {
		enum item items[ITEMS];
		if (cursor < items_shown(items)) {
			item_open(items[cursor]);
		}
	} else if (page == PAGE_WIFI && cursor > 0 && cursor - 1 < wifi.order_count) {
		wifi_pick(&wifi.networks[wifi.order[cursor - 1]]);
	} else if (page == PAGE_BLUETOOTH && cursor > 0 && cursor - 1 < bt.order_count) {
		bluetooth_pick(&bt.devices[bt.order[cursor - 1]]);
	} else if (cursor == 0) {
		row_change(page == PAGE_WIFI ? (wifi.powered ? -1 : 1) : (bt.powered ? -1 : 1));
	}
}

/* Typing a password: printable keys add to it, BackSpace takes back. */
static void secret_key(xkb_keysym_t sym, const char *text) {
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		tell("Connecting to %s…", secret.network);
		secret_answer(true);
	} else if (sym == XKB_KEY_Escape) {
		secret_answer(false);
		tell("Not connected to %s.", secret.network);
	} else if (sym == XKB_KEY_BackSpace) {
		size_t length = strlen(secret.typed);
		while (length > 0 && (secret.typed[length - 1] & 0xc0) == 0x80) {
			length--;
		}
		secret.typed[length > 0 ? length - 1 : 0] = '\0';
	} else if (text[0] != '\0' && strlen(secret.typed) + strlen(text) < sizeof(secret.typed)) {
		strcat(secret.typed, text);
	}
	box_redraw();
}

static void key(xkb_keysym_t sym, const char *text, bool ctrl, bool alt) {
	if (secret.asked) {
		secret_key(sym, text);
		return;
	}
	bool was_confirming = confirming;
	confirming = false;
	status[0] = '\0';
	box_redraw();
	switch (sym) {
	case XKB_KEY_Escape:
		if (was_confirming) {
			break;
		}
		if (page != PAGE_MAIN) {
			page_open(PAGE_MAIN);
		} else {
			box_quit();
		}
		break;
	case XKB_KEY_Up:
		cursor = cursor > 0 ? cursor - 1 : 0;
		break;
	case XKB_KEY_Down:
		cursor = cursor + 1 < page_length() ? cursor + 1 : cursor;
		break;
	case XKB_KEY_Left:
		row_change(-1);
		break;
	case XKB_KEY_Right:
		row_change(+1);
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		confirming = was_confirming;
		row_open();
		break;
	}
}

static void click(int x, int y) {
	int top = border + padding + row_height;
	if (secret.asked || y < top || y >= top + rows * row_height) {
		return;
	}
	size_t row = first + (size_t)((y - top) / row_height);
	if (row < page_length()) {
		cursor = row;
		status[0] = '\0';
		row_open();
		box_redraw();
	}
}

static void scroll(int x, int y, int steps) {
	int top = border + padding + row_height;
	if (secret.asked || y < top || y >= top + rows * row_height) {
		return;
	}
	size_t row = first + (size_t)((y - top) / row_height);
	if (row < page_length()) {
		cursor = row;
		row_change(-steps); /* the wheel up raises */
		box_redraw();
	}
}

/* --- Main -------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0) {
			verbose = true;
		} else {
			fprintf(stderr, "usage: %s [-d]\n"
				"Wi-Fi, Bluetooth, sound, light, battery, power mode and the session,\n"
				"in one box.\n"
				"  -d  say what it does, on stderr\n", argv[0]);
			return 2;
		}
	}
	if (!box_only_one("tw-control", verbose)) {
		return 0;
	}
	sound_read();
	light_read();
	battery_read();
	profile_read();
	bt.driver = bluetooth_driver();

	static const struct box_app app = {
		.name = "control",
		.width = box_width,
		.height = box_height,
		.font = font,
		.draw = draw,
		.key = key,
		.click = click,
		.scroll = scroll,
		.timer = tick,
	};
	if (box_connect(&app, verbose)) {
		if (dbus_open(incoming)) {
			box_watch(dbus_fd(), bus_ready);
			dbus_match("type='signal',sender='" IWD "'");
			dbus_match("type='signal',sender='" BLUEZ "'");
			iwd_refresh();
			bluez_refresh();
		} else {
			say("no system bus");
		}
		clock_timer();
		box_open();
		box_run();
	}
	/* A password half typed is said no to; the agents go with the connection. */
	secret_answer(false);
	bluetooth_look(false);
	dbus_close();
	box_disconnect();
	return 0;
}
