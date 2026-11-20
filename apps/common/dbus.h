/*
 * One connection to a D-Bus bus, for tw's apps: calls with their replies
 * handed to a function, signals and calls from others handed to another, and
 * answers to those. Messages through bus.c; no library.
 */
#ifndef TW_DBUS_H
#define TW_DBUS_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

/* A reply (or error) to a call, NULL if the connection went before it came. */
typedef void (*dbus_reply)(const struct bus_message *reply, void *data);

/* Connects to the system bus ($DBUS_SYSTEM_BUS_ADDRESS, else the usual
 * socket) and says Hello. False if it cannot. Signals and calls to us go to
 * `incoming`. */
bool dbus_open(void (*incoming)(const struct bus_message *message));
/* The socket, to wait on; -1 when not open. */
int dbus_fd(void);
/* Reads what has come and hands it on. False once the bus is gone. */
bool dbus_read(void);
void dbus_close(void);

/* A message is written into the connection's buffer between begin and send;
 * one at a time. */
struct bus_writer *dbus_call(const char *destination, const char *path,
	const char *interface, const char *member, const char *signature);
/* Sends the call; `reply` (if not NULL) gets the answer. */
void dbus_send(dbus_reply reply, void *data);
/* An answer to a call someone made to us, with a body of this signature
 * (NULL for none); then dbus_send(NULL, NULL). */
struct bus_writer *dbus_answer(const struct bus_message *call, const char *signature);
/* An error in answer to a call; sent at once. */
void dbus_answer_error(const struct bus_message *call, const char *error);
/* Asks the bus for the signals matching `rule` ("type='signal',sender=..."). */
void dbus_match(const char *rule);

/* Reading a{sv} property lists: calls `each` with every name and its value,
 * whose type is `type`; what `each` does not read of it is passed over. */
void dbus_properties(struct bus_reader *reader,
	void (*each)(const char *name, const char *type, struct bus_reader *value, void *data),
	void *data);

#endif
