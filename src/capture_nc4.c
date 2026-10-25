/*
 * LG NetCast 4.x capture backend.
 *
 * procmgr composites the TV UI and the apps: every app draws in one of its
 * predefined HOA_GX windows, and the visible ones are merged bottom to top,
 * the lowest one copied and the others blended with their per-pixel alpha.
 * This backend asks procmgr (lg.service.special.processmanagergx on the
 * system bus) for the visible windows and their surfaces, maps the surfaces
 * from /dev/mem and merges them the same way. The video plane (TV picture,
 * hardware-decoded video) is not captured and shows as black.
 */
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "capture.h"

#define GX_SERVICE "lg.service.special.processmanagergx"
#define GX_PATH "/hoa/proc"
#define GX_INTERFACE "lg.if.service"
#define SYSTEM_BUS "unix:path=/usr/local/var/run/dbus/system_bus_socket"
#define CLIENT_NAME "lg.service.normal.vncserver"

#define SCREEN_W 1280
#define SCREEN_H 720
#define MAX_WINDOWS 16
#define MAX_MAPPINGS 16

/* HOA_GX surface info, as returned in hex by HOA_GX_GetWindowSurfaceInfo (back, then front) */
#define SURFACE_INFO_SIZE 1064

typedef struct {
	uint32_t phys;
	uint16_t pitch;
	uint16_t bpp;
	uint16_t width;
	uint16_t height;
} surface_t;

typedef struct {
	uint32_t phys;
	size_t length;
	uint8_t* map;
	uint8_t* pixels;
	int age;
} mapping_t;

static DBusConnection* bus;
static int memfd = -1;
static uint32_t out_w, out_h;
static uint32_t* screen;   /* SCREEN_W x SCREEN_H composite */
static uint32_t* row;      /* one source row, read from uncached memory in one go */
static mapping_t mappings[MAX_MAPPINGS];
static int use_back;

static DBusMessage* gx_call(const char* method, int window) {
	DBusMessage *msg, *reply;
	DBusError err;
	dbus_int32_t pid = getpid();
	const char* name = CLIENT_NAME;

	msg = dbus_message_new_method_call(GX_SERVICE, GX_PATH, GX_INTERFACE, method);
	if (!msg)
		return NULL;
	dbus_message_append_args(msg, DBUS_TYPE_INT32, &pid, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
	if (window >= 0) {
		dbus_int32_t w = window;
		dbus_message_append_args(msg, DBUS_TYPE_INT32, &w, DBUS_TYPE_INVALID);
	}
	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(bus, msg, 1000, &err);
	dbus_message_unref(msg);
	if (!reply) {
		fprintf(stderr, "capture_nc4: %s: %s\n", method, err.message);
		dbus_error_free(&err);
	}
	return reply;
}

/* window IDs from the lowest to the highest Z-order */
static int get_visible_windows(int* ids, int max) {
	DBusMessage* reply = gx_call("HOA_GX_GetVisibleWindowList", -1);
	dbus_int32_t* list = NULL;
	int count = 0, i;
	dbus_bool_t ok = FALSE;

	if (!reply)
		return -1;
	if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_ARRAY, DBUS_TYPE_INT32, &list, &count,
			DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID)) {
		dbus_message_unref(reply);
		return -1;
	}
	/* "false" with an empty list: nothing is visible */
	if (count > max)
		count = max;
	for (i = 0; i < count; i++)
		ids[i] = list[i];
	dbus_message_unref(reply);
	return count;
}

static uint32_t hex_le(const char* s, int bytes) {
	uint32_t v = 0;
	int i;
	char b[3] = { 0 };

	for (i = bytes - 1; i >= 0; i--) {
		b[0] = s[i * 2];
		b[1] = s[i * 2 + 1];
		v = (v << 8) | strtoul(b, NULL, 16);
	}
	return v;
}

static int get_surface(int window, surface_t* surface) {
	DBusMessage* reply = gx_call("HOA_GX_GetWindowSurfaceInfo", window);
	const char* hex = NULL;
	dbus_bool_t ok = FALSE;
	const char* info;

	if (!reply)
		return -1;
	if (!dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &hex, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID) ||
			!ok || strlen(hex) < 4 * SURFACE_INFO_SIZE) {
		dbus_message_unref(reply);
		return -1;
	}
	info = use_back ? hex : hex + 2 * SURFACE_INFO_SIZE;
	surface->phys = hex_le(info + 16, 4);
	surface->pitch = hex_le(info + 24, 2);
	surface->bpp = hex_le(info + 28, 2);
	surface->width = hex_le(info + 32, 2);
	surface->height = hex_le(info + 36, 2);
	dbus_message_unref(reply);
	return 0;
}

static uint8_t* map_surface(const surface_t* s) {
	size_t length = (size_t) s->pitch * s->height;
	uint32_t page = s->phys & ~4095u;
	mapping_t* free_slot = NULL;
	mapping_t* m;
	int i;

	for (i = 0; i < MAX_MAPPINGS; i++) {
		m = &mappings[i];
		if (m->map && m->phys == s->phys && m->length == length) {
			m->age = 0;
			return m->pixels;
		}
		if (!m->map && !free_slot)
			free_slot = m;
	}
	if (!free_slot) {
		/* drop the least recently used mapping */
		free_slot = &mappings[0];
		for (i = 1; i < MAX_MAPPINGS; i++)
			if (mappings[i].age > free_slot->age)
				free_slot = &mappings[i];
		munmap(free_slot->map, free_slot->length + (free_slot->phys - (free_slot->phys & ~4095u)));
		free_slot->map = NULL;
	}
	m = free_slot;
	m->map = mmap(NULL, length + (s->phys - page), PROT_READ, MAP_SHARED, memfd, page);
	if (m->map == MAP_FAILED) {
		fprintf(stderr, "capture_nc4: mmap %08x: %s\n", s->phys, strerror(errno));
		m->map = NULL;
		return NULL;
	}
	m->phys = s->phys;
	m->length = length;
	m->pixels = m->map + (s->phys - page);
	m->age = 0;
	return m->pixels;
}

/*
 * The surfaces are mapped uncached: every load is a bus transaction, so read
 * them with the widest loads available (64 bytes per NEON vldm) instead of
 * memcpy().
 */
static void copy_uncached(void* dst, const void* src, size_t n) {
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
	if ((((uintptr_t) dst | (uintptr_t) src) & 7) == 0) {
		while (n >= 64) {
			__asm__ volatile(
				"vldm %0!, {d0-d7}\n"
				"vstm %1!, {d0-d7}\n"
				: "+r"(src), "+r"(dst)
				:
				: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "memory");
			n -= 64;
		}
	}
#endif
	memcpy(dst, src, n);
}

static void merge(uint32_t* screen, const surface_t* s, const uint8_t* pixels, int copy) {
	uint32_t w = s->width < SCREEN_W ? s->width : SCREEN_W;
	uint32_t h = s->height < SCREEN_H ? s->height : SCREEN_H;
	uint32_t x, y;

	for (y = 0; y < h; y++) {
		uint32_t* dst = screen + y * SCREEN_W;

		if (copy) {
			copy_uncached(dst, pixels + (size_t) y * s->pitch, w * 4);
			continue;
		}
		copy_uncached(row, pixels + (size_t) y * s->pitch, w * 4);
		for (x = 0; x < w; x++) {
			uint32_t src = row[x];
			uint32_t a = src >> 24;

			if (a == 255) {
				dst[x] = src;
			} else if (a != 0) {
				uint32_t d = dst[x];
				uint32_t rb = ((src & 0xff00ff) * a + (d & 0xff00ff) * (255 - a)) >> 8;
				uint32_t g = ((src & 0xff00) * a + (d & 0xff00) * (255 - a)) >> 8;
				dst[x] = (rb & 0xff00ff) | (g & 0xff00) | 0xff000000;
			}
		}
	}
}

int capture_init(uint32_t width, uint32_t height) {
	DBusError err;
	const char* buffer = getenv("NC4_CAPTURE_BUFFER");

	use_back = buffer && strcmp(buffer, "back") == 0;
	out_w = width;
	out_h = height;

	memfd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (memfd < 0) {
		fprintf(stderr, "capture_nc4: /dev/mem: %s\n", strerror(errno));
		return -1;
	}

	dbus_error_init(&err);
	bus = dbus_connection_open_private(SYSTEM_BUS, &err);
	if (!bus || !dbus_bus_register(bus, &err)) {
		fprintf(stderr, "capture_nc4: %s: %s\n", SYSTEM_BUS, err.message);
		dbus_error_free(&err);
		close(memfd);
		memfd = -1;
		return -2;
	}

	screen = calloc(SCREEN_W * SCREEN_H, 4);
	row = malloc(SCREEN_W * 4);
	if (!screen || !row)
		return -3;
	return 0;
}

int capture_execute(uint8_t* target, uint32_t size) {
	int ids[MAX_WINDOWS];
	int count, i, merged = 0;
	uint32_t x, y;
	/* at the native size, merge straight into the target */
	int direct = out_w == SCREEN_W && out_h == SCREEN_H;
	uint32_t* dest = direct ? (uint32_t*) target : screen;

	if (size < out_w * out_h * 4)
		return -1;

	for (i = 0; i < MAX_MAPPINGS; i++)
		mappings[i].age++;

	memset(dest, 0, SCREEN_W * SCREEN_H * 4);
	count = get_visible_windows(ids, MAX_WINDOWS);
	for (i = 0; i < count; i++) {
		surface_t s;
		uint8_t* pixels;

		if (get_surface(ids[i], &s) != 0 || s.bpp != 32 || !s.phys)
			continue;
		if (!(pixels = map_surface(&s)))
			continue;
		merge(dest, &s, pixels, merged == 0);
		merged++;
	}

	if (!direct) {
		uint32_t* dst = (uint32_t*) target;
		for (y = 0; y < out_h; y++) {
			const uint32_t* src = screen + (y * SCREEN_H / out_h) * SCREEN_W;
			for (x = 0; x < out_w; x++)
				*dst++ = src[x * SCREEN_W / out_w];
		}
	}
	return 0;
}

int capture_destroy() {
	int i;

	for (i = 0; i < MAX_MAPPINGS; i++) {
		mapping_t* m = &mappings[i];
		if (m->map)
			munmap(m->map, m->length + (m->phys - (m->phys & ~4095u)));
		m->map = NULL;
	}
	if (bus) {
		dbus_connection_close(bus);
		dbus_connection_unref(bus);
		bus = NULL;
	}
	if (memfd >= 0)
		close(memfd);
	memfd = -1;
	free(screen);
	free(row);
	screen = row = NULL;
	return 0;
}
