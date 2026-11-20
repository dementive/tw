/*
 * tw-launch: starts installed programs. A box in the middle of the screen;
 * type part of a program's name, Return starts the one picked out. The
 * programs are those of the desktop entries in the usual places, the ones
 * started most often first among equal matches.
 *
 * tw starts it for Mod+D, and it is gone again once it has started something
 * or Escape is pressed; pressed again while it is open, the key closes it.
 *
 *   tw-launch                  pick a program and start it
 *   tw-launch --dmenu [-p P]   pick one of the lines on stdin and print it,
 *                              as dmenu does; P is said in front of what you
 *                              type. xdg-desktop-portal-wlr asks with it
 *                              which screen or window to share
 *   -d                         say what it does, on stderr
 *
 * Keys: type to narrow the list; Up, Down, Tab, Ctrl+P, Ctrl+N, Page_Up and
 * Page_Down to move; BackSpace, Ctrl+BackSpace or Ctrl+W (a word), Ctrl+U
 * (all) to take back; Return to start; Escape or Ctrl+C to leave. A click on
 * a program starts it, the wheel moves through the list.
 *
 * No icons, no translated names, no actions of an entry: names and what kind
 * of program each is, drawn with fontconfig and FreeType (apps/common).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"
#include "cursor-shape-v1-client-protocol.h"
#include "draw.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))

static bool verbose, dmenu;
static const char *prompt_text = prompt;

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

_Noreturn static void die(const char *why) {
	fprintf(stderr, "tw-launch: %s\n", why);
	exit(1);
}

static char *copy(const char *text) {
	char *copied = strdup(text);
	if (copied == NULL) {
		die("out of memory");
	}
	return copied;
}

/* --- The programs ------------------------------------------------------------ */

/* A program to start, or with --dmenu a line to pick. */
struct entry {
	char *id;      /* desktop file ID ("org.gnome.Nautilus.desktop"); NULL with --dmenu */
	char *file;    /* where it was read; NULL with --dmenu */
	char *name;    /* what is shown and matched; with --dmenu the line */
	char *generic; /* the kind of program ("Web Browser"), or NULL */
	char *keywords; /* more words to be found by ("internet;www;"), or NULL */
	char *exec;    /* the command line, as the entry gives it */
	char *path;    /* the directory to start it in, or NULL */
	bool terminal, gpu;
	bool shown;    /* false: an entry that hides one of its ID further on */
	int uses;      /* how often it was started from here */
	int score;     /* how well it matches what is typed; 0: not at all */
};

static struct entry *entries;
static size_t entry_count, entry_capacity;

static struct entry *entry_add(void) {
	if (entry_count == entry_capacity) {
		entry_capacity = entry_capacity == 0 ? 256 : entry_capacity * 2;
		entries = realloc(entries, entry_capacity * sizeof(*entries));
		if (entries == NULL) {
			die("out of memory");
		}
	}
	struct entry *entry = &entries[entry_count++];
	*entry = (struct entry){0};
	return entry;
}

static struct entry *entry_find(const char *id) {
	for (size_t i = 0; i < entry_count; i++) {
		if (entries[i].id != NULL && strcmp(entries[i].id, id) == 0) {
			return &entries[i];
		}
	}
	return NULL;
}

/* Undoes a desktop entry's escapes in place: \s \n \t \r \\. */
static void unescape(char *value) {
	char *to = value;
	for (const char *from = value; *from != '\0'; from++) {
		if (*from != '\\' || from[1] == '\0') {
			*to++ = *from;
			continue;
		}
		from++;
		*to++ = *from == 's' ? ' ' : *from == 'n' ? '\n' : *from == 't' ? '\t' :
			*from == 'r' ? '\r' : *from;
	}
	*to = '\0';
}

/* Whether a list such as "GNOME;KDE;" names one of the desktops tw is
 * ("tw", from XDG_CURRENT_DESKTOP). */
static bool names_this_desktop(const char *list) {
	const char *desktops = getenv("XDG_CURRENT_DESKTOP");
	if (desktops == NULL) {
		return false;
	}
	for (const char *at = list; *at != '\0';) {
		size_t length = strcspn(at, ";");
		for (const char *ours = desktops; *ours != '\0';) {
			size_t our_length = strcspn(ours, ":");
			if (length > 0 && length == our_length && strncmp(at, ours, length) == 0) {
				return true;
			}
			ours += our_length + (ours[our_length] == ':');
		}
		at += length + (at[length] == ';');
	}
	return false;
}

/* Whether a program is there to run: a path, or a name found on PATH. */
static bool installed(const char *program) {
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

/* Reads one desktop entry. Only its [Desktop Entry] group counts, and only
 * keys without a language: names are shown as the entry gives them. */
static void entry_read(const char *file, const char *id) {
	if (entry_find(id) != NULL) {
		return; /* one of that ID earlier on the path wins */
	}
	FILE *stream = fopen(file, "re");
	if (stream == NULL) {
		return;
	}
	struct entry *entry = entry_add();
	entry->id = copy(id);
	entry->file = copy(file);
	bool in_group = false, application = false, hidden = false, elsewhere = false;
	char *try_exec = NULL, *line = NULL;
	size_t size = 0;
	while (getline(&line, &size, stream) >= 0) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			if (in_group) {
				break; /* [Desktop Entry] comes first; the rest is actions */
			}
			in_group = strcmp(line, "[Desktop Entry]") == 0;
			continue;
		}
		char *equals = strchr(line, '=');
		if (!in_group || line[0] == '#' || equals == NULL) {
			continue;
		}
		char *key = line, *value = equals + 1;
		for (char *end = equals; end > key && end[-1] == ' '; end--) {
			end[-1] = '\0';
		}
		*equals = '\0';
		value += strspn(value, " ");
		unescape(value);
		char **field = strcmp(key, "Name") == 0 ? &entry->name :
			strcmp(key, "GenericName") == 0 ? &entry->generic :
			strcmp(key, "Keywords") == 0 ? &entry->keywords :
			strcmp(key, "Exec") == 0 ? &entry->exec :
			strcmp(key, "Path") == 0 ? &entry->path :
			strcmp(key, "TryExec") == 0 ? &try_exec : NULL;
		bool yes = strcmp(value, "true") == 0;
		if (field != NULL) {
			free(*field);
			*field = value[0] != '\0' ? copy(value) : NULL;
		} else if (strcmp(key, "Type") == 0) {
			application = strcmp(value, "Application") == 0;
		} else if (strcmp(key, "NoDisplay") == 0 || strcmp(key, "Hidden") == 0) {
			hidden = hidden || yes;
		} else if (strcmp(key, "OnlyShowIn") == 0) {
			elsewhere = elsewhere || !names_this_desktop(value);
		} else if (strcmp(key, "NotShowIn") == 0) {
			elsewhere = elsewhere || names_this_desktop(value);
		} else if (strcmp(key, "Terminal") == 0) {
			entry->terminal = yes;
		} else if (strcmp(key, "PrefersNonDefaultGPU") == 0 ||
				strcmp(key, "X-KDE-RunOnDiscreteGpu") == 0) {
			entry->gpu = entry->gpu || yes;
		}
	}
	free(line);
	fclose(stream);
	entry->shown = application && !hidden && !elsewhere && entry->name != NULL &&
		entry->exec != NULL && (try_exec == NULL || installed(try_exec));
	free(try_exec);
}

/* Reads the entries under `dir`, whose IDs begin with `prefix` (a
 * subdirectory's name and a dash, as the specification has it). */
static void entries_read_dir(const char *dir, const char *prefix) {
	DIR *stream = opendir(dir);
	if (stream == NULL) {
		return;
	}
	struct dirent *found;
	while ((found = readdir(stream)) != NULL) {
		if (found->d_name[0] == '.') {
			continue;
		}
		char path[4096], id[1024];
		if (snprintf(path, sizeof(path), "%s/%s", dir, found->d_name) >= (int)sizeof(path) ||
				snprintf(id, sizeof(id), "%s%s", prefix, found->d_name) >= (int)sizeof(id)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0) {
			continue;
		}
		size_t length = strlen(found->d_name);
		if (S_ISDIR(st.st_mode)) {
			strcat(id, "-");
			entries_read_dir(path, id);
		} else if (length > 8 && strcmp(found->d_name + length - 8, ".desktop") == 0) {
			entry_read(path, id);
		}
	}
	closedir(stream);
}

/* Every desktop entry, the user's own first: $XDG_DATA_HOME/applications,
 * then those of each directory in $XDG_DATA_DIRS. */
static void entries_read(void) {
	char dir[4096];
	const char *home = getenv("XDG_DATA_HOME");
	if (home != NULL && home[0] != '\0') {
		snprintf(dir, sizeof(dir), "%s/applications", home);
	} else {
		snprintf(dir, sizeof(dir), "%s/.local/share/applications", getenv("HOME") ?: "");
	}
	entries_read_dir(dir, "");
	const char *dirs = getenv("XDG_DATA_DIRS");
	if (dirs == NULL || dirs[0] == '\0') {
		dirs = "/usr/local/share:/usr/share";
	}
	while (*dirs != '\0') {
		size_t length = strcspn(dirs, ":");
		if (length > 0 && snprintf(dir, sizeof(dir), "%.*s/applications", (int)length, dirs) <
				(int)sizeof(dir)) {
			entries_read_dir(dir, "");
		}
		dirs += length + (dirs[length] == ':');
	}
}

/* With --dmenu: the lines on stdin. */
static void lines_read(void) {
	char *line = NULL;
	size_t size = 0;
	ssize_t length;
	while ((length = getline(&line, &size, stdin)) >= 0) {
		line[strcspn(line, "\n")] = '\0';
		if (line[0] != '\0') {
			struct entry *entry = entry_add();
			entry->name = copy(line);
			entry->shown = true;
		}
	}
	free(line);
}

/* --- How often each was started ---------------------------------------------- */

/* A line "<uses> <ID>" for each program started from here, in the user's
 * cache directory. Programs no longer installed drop out of it. */
static bool uses_path(char *path, size_t size) {
	const char *cache = getenv("XDG_CACHE_HOME");
	int length = cache != NULL && cache[0] != '\0' ?
		snprintf(path, size, "%s/tw-launch", cache) :
		snprintf(path, size, "%s/.cache/tw-launch", getenv("HOME") ?: "");
	return length > 0 && (size_t)length < size;
}

static void uses_read(void) {
	char path[4096];
	FILE *stream = uses_path(path, sizeof(path)) ? fopen(path, "re") : NULL;
	if (stream == NULL) {
		return;
	}
	int uses;
	char id[1024];
	while (fscanf(stream, "%d %1023s", &uses, id) == 2) {
		struct entry *entry = entry_find(id);
		if (entry != NULL && uses > 0) {
			entry->uses = uses;
		}
	}
	fclose(stream);
}

/* Counts one more start of `started` and writes the file anew, whole or not
 * at all: to a new file that then takes the old one's place. */
static void uses_write(struct entry *started) {
	char path[4096], temporary[4200];
	if (!uses_path(path, sizeof(path))) {
		return;
	}
	started->uses++;
	char *slash = strrchr(path, '/');
	*slash = '\0';
	mkdir(path, 0700);
	*slash = '/';
	snprintf(temporary, sizeof(temporary), "%s.%d", path, (int)getpid());
	FILE *stream = fopen(temporary, "we");
	if (stream == NULL) {
		return;
	}
	for (size_t i = 0; i < entry_count; i++) {
		if (entries[i].uses > 0 && entries[i].shown) {
			fprintf(stream, "%d %s\n", entries[i].uses, entries[i].id);
		}
	}
	if (fclose(stream) != 0 || rename(temporary, path) != 0) {
		unlink(temporary);
	}
}

/* --- Matching ---------------------------------------------------------------- */

static char query[256]; /* what is typed */
static size_t query_length;
static size_t *matches; /* indices into entries, best first */
static size_t match_count;
static size_t selected; /* in `matches`: what Return starts */
static size_t first;    /* in `matches`: the one shown at the top */

static bool word_start(const char *text, const char *at) {
	if (at == text) {
		return true;
	}
	char before = at[-1];
	return !(before >= 'a' && before <= 'z') && !(before >= 'A' && before <= 'Z') &&
		!(before >= '0' && before <= '9');
}

/* How well `text` matches the query: 4 it begins with it, 3 a word in it
 * does, 2 it holds it, 1 it holds its characters in order; 0 not at all.
 * Upper and lower case are the same. */
static int match_text(const char *text) {
	if (text == NULL) {
		return 0;
	}
	if (strncasecmp(text, query, query_length) == 0) {
		return 4;
	}
	int found = 0;
	for (const char *at = strcasestr(text, query); at != NULL; at = strcasestr(at + 1, query)) {
		if (word_start(text, at)) {
			return 3;
		}
		found = 2;
	}
	if (found > 0) {
		return found;
	}
	const char *wanted = query;
	for (const char *at = text; *at != '\0' && *wanted != '\0'; at++) {
		char a = *at, b = *wanted;
		if ((a >= 'A' && a <= 'Z' ? a + 32 : a) == (b >= 'A' && b <= 'Z' ? b + 32 : b)) {
			wanted++;
		}
	}
	return *wanted == '\0' ? 1 : 0;
}

/* The name counts most, then the kind of program and its keywords ("browser"
 * finds Firefox), then the name's characters in order. */
static int entry_score(const struct entry *entry) {
	if (!entry->shown) {
		return 0;
	}
	if (query_length == 0) {
		return 1;
	}
	int name = match_text(entry->name);
	if (name >= 2) {
		return name + 2;
	}
	int other = match_text(entry->generic) >= 2 || match_text(entry->keywords) >= 2;
	return other ? 3 : name == 1 ? 2 : 0;
}

/* Best match first; among equals the most started, then by name. */
static int match_order(const void *a, const void *b) {
	const struct entry *x = &entries[*(const size_t *)a], *y = &entries[*(const size_t *)b];
	if (x->score != y->score) {
		return y->score - x->score;
	}
	if (x->uses != y->uses) {
		return y->uses - x->uses;
	}
	int names = strcasecmp(x->name, y->name);
	return names != 0 ? names : (x < y ? -1 : 1);
}

static void matches_update(void) {
	if (matches == NULL && entry_count > 0) {
		matches = calloc(entry_count, sizeof(*matches));
		if (matches == NULL) {
			die("out of memory");
		}
	}
	match_count = 0;
	for (size_t i = 0; i < entry_count; i++) {
		entries[i].score = entry_score(&entries[i]);
		if (entries[i].score > 0) {
			matches[match_count++] = i;
		}
	}
	/* --dmenu keeps the order it was given. */
	if (!dmenu) {
		qsort(matches, match_count, sizeof(*matches), match_order);
	} else if (query_length > 0) {
		for (size_t i = 1; i < match_count; i++) {
			size_t moved = matches[i], at = i;
			for (; at > 0 && entries[matches[at - 1]].score < entries[moved].score; at--) {
				matches[at] = matches[at - 1];
			}
			matches[at] = moved;
		}
	}
	selected = first = 0;
	if (verbose) {
		fprintf(stderr, "matches %zu:", match_count);
		for (size_t i = 0; i < match_count && i < rows; i++) {
			fprintf(stderr, "%s%s", i == 0 ? " " : "|", entries[matches[i]].name);
		}
		fputc('\n', stderr);
	}
}

/* Moves the selection by `by` (wrapping round at either end when it was
 * there already) and scrolls it into view. */
static void select_by(long by) {
	if (match_count == 0) {
		return;
	}
	long to = (long)selected + by, last = (long)match_count - 1;
	if (to < 0) {
		to = selected == 0 ? last : 0;
	} else if (to > last) {
		to = (long)selected == last ? 0 : last;
	}
	selected = (size_t)to;
	if (selected < first) {
		first = selected;
	} else if (selected >= first + rows) {
		first = selected - rows + 1;
	}
	say("selected %s", entries[matches[selected]].name);
}

/* --- Starting a program ------------------------------------------------------ */

static void args_add(char ***argv, size_t *count, char *arg) {
	*argv = realloc(*argv, (*count + 2) * sizeof(**argv));
	if (*argv == NULL) {
		die("out of memory");
	}
	(*argv)[(*count)++] = arg;
	(*argv)[*count] = NULL;
}

/*
 * The arguments of an entry's Exec line: split at spaces, with double quotes
 * keeping one together (\" \` \$ \\ inside them stand for the character).
 * The field codes for files and URLs (%f %F %u %U) and the icon (%i) go,
 * %c is the name, %k the entry's file, %% a percent sign.
 */
static char **exec_args(const struct entry *entry) {
	char **argv = NULL;
	size_t count = 0;
	const char *at = entry->exec;
	while (*at != '\0') {
		at += strspn(at, " \t");
		if (*at == '\0') {
			break;
		}
		/* Each field code may grow into the name or the file. */
		size_t codes = 0, longest = strlen(entry->name) > strlen(entry->file) ?
			strlen(entry->name) : strlen(entry->file);
		for (const char *c = strchr(at, '%'); c != NULL; c = strchr(c + 1, '%')) {
			codes++;
		}
		size_t capacity = strlen(at) + codes * longest + 1;
		char *arg = malloc(capacity), *to = arg;
		if (arg == NULL) {
			die("out of memory");
		}
		bool quoted = false, code = false;
		for (; *at != '\0' && (quoted || (*at != ' ' && *at != '\t')); at++) {
			if (*at == '"') {
				quoted = !quoted;
			} else if (quoted && *at == '\\' && at[1] != '\0' && strchr("\"`$\\", at[1])) {
				*to++ = *++at;
			} else if (*at == '%' && at[1] != '\0') {
				char field = *++at;
				code = code || strchr("fFuUi", field) != NULL;
				const char *with = field == '%' ? "%" : field == 'c' ? entry->name :
					field == 'k' ? entry->file : "";
				to = stpcpy(to, with);
			} else {
				*to++ = *at;
			}
		}
		*to = '\0';
		/* An argument that was only a field code for files goes whole. */
		if (code && arg[0] == '\0') {
			free(arg);
		} else {
			args_add(&argv, &count, arg);
		}
	}
	return argv;
}

/*
 * Starts the program, in a session of its own so that it outlives this one,
 * with the activation token (if any) that lets its window take the focus.
 * One that wants a terminal gets one, and one that wants the discrete GPU
 * goes through tw-gpu.
 */
static void entry_start(struct entry *entry, const char *token) {
	char **argv = exec_args(entry);
	if (argv == NULL) {
		say("nothing to run for %s", entry->id);
		return;
	}
	char **command = NULL;
	size_t count = 0;
	if (entry->terminal) {
		args_add(&command, &count, "/bin/sh");
		args_add(&command, &count, "-c");
		args_add(&command, &count, (char *)terminal);
		args_add(&command, &count, "sh");
	}
	for (char **arg = argv; *arg != NULL; arg++) {
		args_add(&command, &count, *arg);
	}
	char **through_gpu = NULL;
	size_t gpu_count = 0;
	for (size_t i = 0; entry->gpu && i < LENGTH(gpu_command); i++) {
		args_add(&through_gpu, &gpu_count, (char *)gpu_command[i]);
	}
	for (size_t i = 0; entry->gpu && i < count; i++) {
		args_add(&through_gpu, &gpu_count, command[i]);
	}
	if (verbose) {
		fprintf(stderr, "start %s:", entry->id);
		for (char **arg = through_gpu != NULL ? through_gpu : command; *arg != NULL; arg++) {
			fprintf(stderr, " [%s]", *arg);
		}
		fputc('\n', stderr);
	}
	uses_write(entry);

	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGPIPE, SIG_DFL);
		int null = open("/dev/null", O_RDONLY);
		if (null > STDIN_FILENO) {
			dup2(null, STDIN_FILENO);
			close(null);
		}
		if (token != NULL) {
			setenv("XDG_ACTIVATION_TOKEN", token, 1);
			setenv("DESKTOP_STARTUP_ID", token, 1);
		}
		if (entry->path != NULL && chdir(entry->path) != 0) {
			fprintf(stderr, "tw-launch: %s: no directory %s\n", entry->id, entry->path);
		}
		if (through_gpu != NULL && installed(through_gpu[0])) {
			execvp(through_gpu[0], through_gpu);
		}
		execvp(command[0], command);
		fprintf(stderr, "tw-launch: cannot start %s: %s\n", command[0], strerror(errno));
		_exit(127);
	}
	if (pid < 0) {
		fprintf(stderr, "tw-launch: cannot start %s: %s\n", entry->id, strerror(errno));
	}
	free(through_gpu);
	free(command);
	for (char **arg = argv; *arg != NULL; arg++) {
		free(*arg);
	}
	free(argv);
}

/* --- Drawing ----------------------------------------------------------------- */

static uint32_t scale = 120; /* of the screen the box is on, in 120ths */
static bool dirty;           /* the box does not show what it should */

static int px(int logical) {
	return (logical * (int)scale + 60) / 120;
}

/* The box's size, in logical pixels: the line typed in and `rows` below it. */
static int box_height(void) {
	return 2 * border + 2 * padding + row_height * (1 + rows) + padding / 2;
}

/* As much of `text` as fits from x up to `right`, with "…" if it is cut.
 * Returns where the pen ends. */
static int text_line(struct picture *picture, int x, int right, int baseline,
		const char *text, uint32_t colour) {
	int width = (right - x) * 64;
	size_t length = text_fit(text, width, false);
	bool cut = text[length] != '\0';
	if (cut) {
		length = text_fit(text, width - text_advance(0x2026), false);
	}
	return text_draw(picture, x, baseline, text, length, cut, colour);
}

static void box_draw(struct picture *picture) {
	int edge = px(border), inside = edge + px(padding), row = px(row_height);
	int right = picture->width - inside;
	picture_fill(picture, 0, 0, picture->width, picture->height, colour_border);
	picture_fill(picture, edge, edge, picture->width - 2 * edge, picture->height - 2 * edge,
		colour_background);
	int lift = (row - font_line_height()) / 2 + font_ascent();

	/* The line typed in: the prompt, then the end of what is typed that
	 * fits, then the cursor. */
	int x = text_line(picture, inside, right, inside + lift, prompt_text, colour_prompt);
	const char *shown = query;
	int room = (right - x - px(2)) * 64;
	while (*shown != '\0' && text_fit(shown, room, false) < strlen(shown)) {
		text_character(&shown);
	}
	x = text_draw(picture, x, inside + lift, shown, strlen(shown), false, colour_text);
	picture_fill(picture, x + px(1), inside + (row - font_line_height()) / 2, px(2) > 0 ? px(2) : 1,
		font_line_height(), colour_text);

	int top = inside + row + px(padding / 2);
	for (size_t i = first; i < match_count && i < first + rows; i++, top += row) {
		const struct entry *entry = &entries[matches[i]];
		bool picked = i == selected;
		if (picked) {
			picture_fill(picture, edge, top, picture->width - 2 * edge, row, colour_selected);
		}
		int end = text_line(picture, inside, right, top + lift, entry->name,
			picked ? colour_selected_text : colour_text);
		/* Its kind, unless that is all its name says ("Bulk Rename"). */
		if (entry->generic != NULL && strcasecmp(entry->generic, entry->name) != 0 &&
				end + px(16) < right) {
			text_line(picture, end + px(12), right, top + lift, entry->generic, colour_hint);
		}
	}
}

/* --- The box ----------------------------------------------------------------- */

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wp_viewporter *viewporter;
static struct wp_fractional_scale_manager_v1 *scale_manager;
static struct xdg_activation_v1 *activation;
static struct wp_cursor_shape_manager_v1 *cursor_shapes;

static struct wl_registry *registry;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wp_viewport *viewport;
static struct wp_fractional_scale_v1 *fractional;
static struct wl_buffer *buffer;
static bool configured;
static bool running = true;
static int status = 1; /* 0 once something was picked */

static void box_redraw(void) {
	dirty = false;
	if (!configured) {
		return;
	}
	struct picture picture;
	if (!picture_open(&picture, px(box_width), px(box_height()))) {
		die("no memory for the picture");
	}
	font_size_set(px(font_size));
	box_draw(&picture);
	struct wl_buffer *drawn = picture_close(&picture, shm);
	wl_surface_attach(surface, drawn, 0, 0);
	if (viewport != NULL) {
		wp_viewport_set_destination(viewport, box_width, box_height());
	}
	wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface);
	if (buffer != NULL) {
		wl_buffer_destroy(buffer);
	}
	buffer = drawn;
	say("draw %dx%d", px(box_width), px(box_height()));
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
		uint32_t width, uint32_t height) {
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	if (!configured) {
		configured = true;
		dirty = true;
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
	/* The screen it was on went away. */
	running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void scale_preferred(void *data, struct wp_fractional_scale_v1 *object,
		uint32_t value) {
	if (value != scale && value != 0) {
		scale = value;
		dirty = true;
	}
}

static const struct wp_fractional_scale_v1_listener scale_listener = {
	.preferred_scale = scale_preferred,
};

/* What was picked is started (or with --dmenu printed) once the compositor
 * has made a token from the key or click that picked it. */
static struct entry *picking;

static void finish(const char *token) {
	running = false;
	status = 0;
	if (dmenu) {
		puts(picking != NULL ? picking->name : query);
		fflush(stdout);
	} else if (picking != NULL) {
		entry_start(picking, token);
	}
}

static void token_done(void *data, struct xdg_activation_token_v1 *made, const char *token) {
	xdg_activation_token_v1_destroy(made);
	finish(token);
}

static const struct xdg_activation_token_v1_listener token_listener = {
	.done = token_done,
};

/* Picks the selected match: Return, or a click. With --dmenu and nothing
 * matching, what is typed is the answer, as with dmenu. */
static void pick(uint32_t serial) {
	if (picking != NULL || !running) {
		return;
	}
	if (match_count == 0 && (!dmenu || query_length == 0)) {
		return;
	}
	picking = match_count > 0 ? &entries[matches[selected]] : NULL;
	if (dmenu || activation == NULL) {
		finish(NULL);
		return;
	}
	struct xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(activation);
	xdg_activation_token_v1_add_listener(token, &token_listener, NULL);
	xdg_activation_token_v1_set_serial(token, serial, seat);
	xdg_activation_token_v1_set_surface(token, surface);
	xdg_activation_token_v1_commit(token);
}

/* --- Keys -------------------------------------------------------------------- */

static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *xkb_state;
static int repeat_fd = -1;
static int repeat_rate = 25, repeat_delay = 600;
static uint32_t repeat_key, last_serial;

/* Takes back the last character, or with `word` the last word and the
 * spaces after it. */
static void query_erase(bool word) {
	size_t end = query_length;
	while (word && end > 0 && query[end - 1] == ' ') {
		end--;
	}
	do {
		while (end > 0 && (query[end - 1] & 0xc0) == 0x80) {
			end--; /* the rest of a character of several bytes */
		}
		if (end > 0) {
			end--;
		}
	} while (word && end > 0 && query[end - 1] != ' ');
	if (end != query_length) {
		query_length = end;
		query[end] = '\0';
		matches_update();
	}
}

static void key_handle(uint32_t code) {
	xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state, code);
	bool ctrl = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL,
		XKB_STATE_MODS_EFFECTIVE) > 0;
	bool alt = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_ALT,
		XKB_STATE_MODS_EFFECTIVE) > 0;
	dirty = true;
	switch (sym) {
	case XKB_KEY_Escape:
		running = false;
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		pick(last_serial);
		return;
	case XKB_KEY_Up:
	case XKB_KEY_ISO_Left_Tab:
		select_by(-1);
		return;
	case XKB_KEY_Down:
	case XKB_KEY_Tab:
		select_by(+1);
		return;
	case XKB_KEY_Page_Up:
		select_by(-(long)rows);
		return;
	case XKB_KEY_Page_Down:
		select_by(+(long)rows);
		return;
	case XKB_KEY_BackSpace:
		query_erase(ctrl);
		return;
	}
	if (ctrl) {
		switch (sym) {
		case XKB_KEY_c:
		case XKB_KEY_g:
		case XKB_KEY_bracketleft:
			running = false;
			break;
		case XKB_KEY_p:
			select_by(-1);
			break;
		case XKB_KEY_n:
			select_by(+1);
			break;
		case XKB_KEY_w:
			query_erase(true);
			break;
		case XKB_KEY_u:
			query_length = 0;
			query[0] = '\0';
			matches_update();
			break;
		}
		return;
	}
	char text[16];
	int length = xkb_state_key_get_utf8(xkb_state, code, text, sizeof(text));
	if (alt || length <= 0 || (unsigned char)text[0] < 0x20 || text[0] == 0x7f ||
			query_length + (size_t)length >= sizeof(query)) {
		dirty = false;
		return;
	}
	memcpy(query + query_length, text, (size_t)length + 1);
	query_length += (size_t)length;
	matches_update();
}

static void repeat_arm(uint32_t code) {
	repeat_key = code;
	struct itimerspec spec = {0};
	if (code != 0 && repeat_rate > 0) {
		spec.it_value.tv_sec = repeat_delay / 1000;
		spec.it_value.tv_nsec = (long)(repeat_delay % 1000) * 1000000;
		spec.it_interval.tv_nsec = 1000000000L / repeat_rate;
		if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
			spec.it_value.tv_nsec = 1;
		}
	}
	if (repeat_fd >= 0) {
		timerfd_settime(repeat_fd, 0, &spec, NULL);
	}
}

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format,
		int32_t fd, uint32_t size) {
	char *text = format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 ?
		mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	close(fd);
	if (text == MAP_FAILED) {
		return;
	}
	struct xkb_keymap *made = xkb_keymap_new_from_string(xkb, text,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(text, size);
	if (made == NULL) {
		return;
	}
	xkb_state_unref(xkb_state);
	xkb_keymap_unref(keymap);
	keymap = made;
	xkb_state = xkb_state_new(keymap);
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		struct wl_surface *entered, struct wl_array *keys) {
	last_serial = serial;
	say("keyboard");
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		struct wl_surface *left) {
	repeat_arm(0);
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	if (xkb_state == NULL) {
		return;
	}
	uint32_t code = key + 8; /* evdev to XKB */
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		last_serial = serial;
		key_handle(code);
		repeat_arm(xkb_keymap_key_repeats(keymap, code) ? code : 0);
	} else if (code == repeat_key) {
		repeat_arm(0);
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
	if (xkb_state != NULL) {
		xkb_state_update_mask(xkb_state, depressed, latched, locked, 0, 0, group);
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
	repeat_rate = rate;
	repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

/* --- The mouse --------------------------------------------------------------- */

static wl_fixed_t pointer_y;
static struct wp_cursor_shape_device_v1 *cursor;

/* The match under the pointer, if there is one. */
static bool match_at(size_t *found) {
	int y = (int)(wl_fixed_to_double(pointer_y) * scale / 120);
	int top = px(border) + px(padding) + px(row_height) + px(padding / 2);
	if (y < top) {
		return false;
	}
	size_t i = first + (size_t)((y - top) / px(row_height));
	if (i >= match_count || i >= first + rows) {
		return false;
	}
	*found = i;
	return true;
}

static void pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		struct wl_surface *entered, wl_fixed_t x, wl_fixed_t y) {
	pointer_y = y;
	if (cursor == NULL && cursor_shapes != NULL) {
		cursor = wp_cursor_shape_manager_v1_get_pointer(cursor_shapes, wl_pointer);
	}
	if (cursor != NULL) {
		wp_cursor_shape_device_v1_set_shape(cursor, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
	}
}

static void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		struct wl_surface *left) {
}

static void pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
	pointer_y = y;
}

static void pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	size_t i;
	if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED && match_at(&i)) {
		selected = i;
		dirty = true;
		pick(serial);
	}
}

static void pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && value != 0) {
		select_by(value > 0 ? +1 : -1);
		dirty = true;
	}
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

/* --- Setup ------------------------------------------------------------------- */

static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && keyboard == NULL) {
		keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && pointer == NULL) {
		pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void registry_global(void *data, struct wl_registry *from, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0 && version >= 4) {
		compositor = wl_registry_bind(from, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(from, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && seat == NULL && version >= 4) {
		/* 4: the keyboard says how keys repeat. */
		seat = wl_registry_bind(from, name, &wl_seat_interface, 4);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(from, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(from, name, &wp_viewporter_interface, 1);
	} else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		scale_manager = wl_registry_bind(from, name,
			&wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
		activation = wl_registry_bind(from, name, &xdg_activation_v1_interface, 1);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		cursor_shapes = wl_registry_bind(from, name,
			&wp_cursor_shape_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *from, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void box_open(void) {
	surface = wl_compositor_create_surface(compositor);
	/* On the overlay layer, above a fullscreen window too, taking every key
	 * while it is open. No anchor: the compositor centres it, and no
	 * output named: on the screen in use. */
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "launcher");
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(layer_surface, box_width, (uint32_t)box_height());
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 1);
	if (viewporter != NULL && scale_manager != NULL) {
		viewport = wp_viewporter_get_viewport(viewporter, surface);
		fractional = wp_fractional_scale_manager_v1_get_fractional_scale(scale_manager, surface);
		wp_fractional_scale_v1_add_listener(fractional, &scale_listener, NULL);
	}
	wl_surface_commit(surface);
}

/*
 * One launcher at a time: the key that opened it closes it again. The first
 * holds a lock on a file in the runtime directory with its pid in it; a
 * second finds it held, ends the first and leaves.
 */
static bool only_one(void) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char path[4096];
	if (runtime == NULL || snprintf(path, sizeof(path), "%s/tw-launch.pid", runtime) >=
			(int)sizeof(path)) {
		return true;
	}
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) {
		return true;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		char text[32] = "";
		ssize_t length = read(fd, text, sizeof(text) - 1);
		long pid = length > 0 ? strtol(text, NULL, 10) : 0;
		if (pid > 0) {
			kill((pid_t)pid, SIGTERM);
			say("closed the one that was open");
		}
		close(fd);
		return false;
	}
	char text[32];
	int length = snprintf(text, sizeof(text), "%d\n", (int)getpid());
	if (ftruncate(fd, 0) != 0 || write(fd, text, (size_t)length) != length) {
		say("could not write %s", path);
	}
	return true; /* fd stays open, and the lock with it, until the exit */
}

/* Lets go of everything: the box and the connection, the keymap, the font,
 * the programs read. */
static void quit(void) {
	struct wl_proxy *proxies[] = {
		(struct wl_proxy *)buffer, (struct wl_proxy *)fractional, (struct wl_proxy *)viewport,
		(struct wl_proxy *)layer_surface, (struct wl_proxy *)surface, (struct wl_proxy *)cursor,
		(struct wl_proxy *)pointer, (struct wl_proxy *)keyboard, (struct wl_proxy *)seat,
		(struct wl_proxy *)compositor, (struct wl_proxy *)shm, (struct wl_proxy *)layer_shell,
		(struct wl_proxy *)viewporter, (struct wl_proxy *)scale_manager,
		(struct wl_proxy *)activation, (struct wl_proxy *)cursor_shapes,
		(struct wl_proxy *)registry,
	};
	/* Only our side of each: the compositor drops its own with the
	 * connection. What was asked of it last (a token) is answered by now. */
	for (size_t i = 0; i < LENGTH(proxies); i++) {
		if (proxies[i] != NULL) {
			wl_proxy_destroy(proxies[i]);
		}
	}
	wl_display_flush(display);
	wl_display_disconnect(display);
	xkb_state_unref(xkb_state);
	xkb_keymap_unref(keymap);
	xkb_context_unref(xkb);
	font_close();
	for (size_t i = 0; i < entry_count; i++) {
		struct entry *entry = &entries[i];
		char *fields[] = {entry->id, entry->file, entry->name, entry->generic,
			entry->keywords, entry->exec, entry->path};
		for (size_t j = 0; j < LENGTH(fields); j++) {
			free(fields[j]);
		}
	}
	free(entries);
	free(matches);
}

static void usage(const char *name) {
	fprintf(stderr, "usage: %s [-d] [--dmenu [-p prompt]]\n"
		"Starts a program: type part of its name, Return starts it.\n"
		"  --dmenu  pick one of the lines on stdin and print it instead\n"
		"  -p       what is said in front of what you type\n"
		"  -d       say what it does, on stderr\n", name);
	exit(2);
}

int main(int argc, char *argv[]) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0) {
			verbose = true;
		} else if (strcmp(argv[i], "--dmenu") == 0 || strcmp(argv[i], "-dmenu") == 0) {
			dmenu = true;
		} else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
			prompt_text = argv[++i];
		} else {
			usage(argv[0]);
		}
	}
	if (dmenu) {
		lines_read();
	} else {
		if (!only_one()) {
			return 0;
		}
		entries_read();
		uses_read();
	}
	matches_update();

	xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	display = wl_display_connect(NULL);
	if (xkb == NULL || display == NULL) {
		die("no Wayland compositor to show the launcher on");
	}
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0 || compositor == NULL || shm == NULL ||
			seat == NULL || layer_shell == NULL) {
		die("this compositor has no layer shell");
	}
	font_open(font, verbose);
	box_open();

	while (running) {
		if (dirty) {
			box_redraw();
		}
		if (wl_display_flush(display) < 0 && errno != EAGAIN) {
			break;
		}
		struct pollfd fds[2] = {
			{wl_display_get_fd(display), POLLIN, 0},
			{repeat_fd, POLLIN, 0},
		};
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (fds[0].revents != 0 && wl_display_dispatch(display) < 0) {
			break;
		}
		uint64_t fired;
		if ((fds[1].revents & POLLIN) != 0 &&
				read(repeat_fd, &fired, sizeof(fired)) == sizeof(fired) && repeat_key != 0) {
			while (fired-- > 0 && running) {
				key_handle(repeat_key);
			}
		}
	}
	quit();
	return status;
}
