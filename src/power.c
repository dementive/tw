/*
 * Running on battery: the outputs drop to a slower mode and a command sets
 * the machine to save power, and both are undone when the power cable is
 * back (see battery_refresh_hz in config.h). As the battery runs low tw
 * gives a warning, and then suspends the machine.
 *
 * The kernel announces every change of a power supply, and udev passes that
 * on to whoever asked for that kind of device. So this costs one socket that
 * is silent until a cable moves or the battery reports a new level, and
 * nothing at all on a machine without a battery. tw does not read what the
 * announcement says: it looks at the supplies again and acts if the answer
 * changed.
 */
#include <dirent.h>
#include <fcntl.h>
#include <libudev.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "tw.h"
#include "config.h"

struct tw_power {
	struct udev *udev;
	struct udev_monitor *monitor;
	struct wl_event_source *source;
	struct tw_battery battery;
	bool fresh;        /* the next look at the charge is the first of its kind */
	int64_t asleep_ms; /* how long the machine had slept at the last look */
	char dir[];        /* where the kernel lists the power supplies */
};

/* One attribute of a power supply without its newline, or "" if it has none. */
static void attribute(const char *dir, const char *supply, const char *name,
		char *value, size_t size) {
	char path[PATH_MAX];
	value[0] = '\0';
	if (snprintf(path, sizeof(path), "%s/%s/%s", dir, supply, name) >= (int)sizeof(path)) {
		return;
	}
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return;
	}
	ssize_t length = read(fd, value, size - 1);
	close(fd);
	if (length > 0) {
		value[length] = '\0';
		value[strcspn(value, "\n")] = '\0';
	}
}

/*
 * Whether the machine has a battery of its own, and whether anything else
 * supplies it. This is the kernel's own rule
 * (power_supply_is_system_supplied()): a supply of scope "Device" belongs to
 * a mouse or a phone and is left out; of the rest, any that is not a battery
 * and is online means mains, be it the charger or a USB-C port.
 *
 * `charge` is how full the fullest battery is, in percent: with two, the
 * machine runs on until both are empty. -1 if none says.
 */
static void supplies_read(const char *dir, bool *battery, bool *mains, int *charge) {
	*battery = *mains = false;
	*charge = -1;
	DIR *supplies = opendir(dir);
	if (supplies == NULL) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(supplies)) != NULL) {
		char type[32], scope[32], value[16];
		if (entry->d_name[0] == '.') {
			continue;
		}
		attribute(dir, entry->d_name, "scope", scope, sizeof(scope));
		if (strcmp(scope, "Device") == 0) {
			continue;
		}
		attribute(dir, entry->d_name, "type", type, sizeof(type));
		if (strcmp(type, "Battery") == 0) {
			*battery = true;
			attribute(dir, entry->d_name, "capacity", value, sizeof(value));
			if (value[0] >= '0' && value[0] <= '9' && atoi(value) > *charge) {
				*charge = atoi(value);
			}
		} else if (type[0] != '\0') {
			attribute(dir, entry->d_name, "online", value, sizeof(value));
			*mains = *mains || strcmp(value, "1") == 0;
		}
	}
	closedir(supplies);
}

/* Brings the outputs and the machine in line with server->on_battery. */
static void apply(struct tw_server *server) {
	wlr_log(WLR_INFO, "running on %s", server->on_battery ? "battery" : "mains");
	outputs_update_mode(server);
	spawn_installed(server->on_battery ? battery_command : mains_command);
}

/*
 * How long the machine has been asleep since it booted: one clock stops
 * while it sleeps and the other does not. If that has grown between two
 * looks, the machine has just woken up.
 */
static int64_t asleep_ms(void) {
	struct timespec all, awake;
	clock_gettime(CLOCK_BOOTTIME, &all);
	clock_gettime(CLOCK_MONOTONIC, &awake);
	return (int64_t)(all.tv_sec - awake.tv_sec) * 1000 +
		(all.tv_nsec - awake.tv_nsec) / 1000000;
}

static int handle_change(int fd, uint32_t mask, void *data) {
	struct tw_server *server = data;
	struct tw_power *power = server->power;
	struct udev_device *device;
	/* Several announcements can be waiting, and one look answers them all. */
	while ((device = udev_monitor_receive_device(power->monitor)) != NULL) {
		udev_device_unref(device);
	}

	bool battery, mains;
	int charge;
	supplies_read(power->dir, &battery, &mains, &charge);
	wlr_log(WLR_DEBUG, "a power supply changed: battery %d, mains %d, charge %d",
		battery, mains, charge);
	if ((battery && !mains) != server->on_battery) {
		server->on_battery = !server->on_battery;
		power->battery = (struct tw_battery){0};
		power->fresh = true;
		apply(server);
	}

	int64_t asleep = asleep_ms();
	if (asleep - power->asleep_ms > 500) {
		power->fresh = true;
	}
	power->asleep_ms = asleep;
	if (!server->on_battery || charge < 0) {
		return 0;
	}

	int due = tw_battery_look(&power->battery, charge, power->fresh,
		battery_warn_percent, battery_suspend_percent);
	power->fresh = false;
	if (due & TW_BATTERY_WARN) {
		wlr_log(WLR_INFO, "battery at %d%%, warning", charge);
		spawn_installed(battery_warn_command);
	}
	if (due & TW_BATTERY_SUSPEND) {
		wlr_log(WLR_INFO, "battery at %d%%, suspending", charge);
		suspend_now(server);
	}
	return 0;
}

void power_init(struct tw_server *server) {
	/* A tw inside another session, or one without a screen, is not what
	 * runs the machine: its power is that session's business. The tests say
	 * otherwise by pointing TW_SYS at a made-up machine. */
	const char *sys = getenv("TW_SYS");
	if (server->session == NULL && sys == NULL) {
		return;
	}
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/class/power_supply", sys != NULL ? sys : "/sys");

	bool battery, mains;
	int charge;
	supplies_read(dir, &battery, &mains, &charge);
	if (!battery) {
		return;
	}
	struct tw_power *power = calloc(1, sizeof(*power) + strlen(dir) + 1);
	if (power == NULL) {
		return;
	}
	strcpy(power->dir, dir);
	server->power = power;

	/* What the kernel announces is about the real machine, not a made-up
	 * one: a test that heard the real battery would act on its own. */
	power->udev = sys == NULL ? udev_new() : NULL;
	if (power->udev != NULL) {
		power->monitor = udev_monitor_new_from_netlink(power->udev, "udev");
	}
	if (power->monitor != NULL &&
			udev_monitor_filter_add_match_subsystem_devtype(power->monitor,
				"power_supply", NULL) >= 0 &&
			udev_monitor_enable_receiving(power->monitor) >= 0) {
		power->source = wl_event_loop_add_fd(server->loop,
			udev_monitor_get_fd(power->monitor), WL_EVENT_READABLE, handle_change, server);
	}
	if (power->source == NULL && sys == NULL) {
		/* What holds now is still acted on below. */
		wlr_log(WLR_ERROR, "cannot watch the power supplies: "
			"plugging the power cable in or out will go unnoticed");
	}

	/* Also when on mains: an earlier session may have ended on battery and
	 * left the machine saving power. The charge waits for the battery's next
	 * announcement: a warning now would come before the session is up to
	 * show it. */
	server->on_battery = !mains;
	power->fresh = true;
	power->asleep_ms = asleep_ms();
	apply(server);
}

void power_finish(struct tw_server *server) {
	struct tw_power *power = server->power;
	if (power == NULL) {
		return;
	}
	if (power->source != NULL) {
		wl_event_source_remove(power->source);
	}
	if (power->monitor != NULL) {
		udev_monitor_unref(power->monitor);
	}
	if (power->udev != NULL) {
		udev_unref(power->udev);
	}
	free(power);
	server->power = NULL;
}
