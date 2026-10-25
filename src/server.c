#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>

#include "log.h"
#include "server.h"
#include "settings.h"
#include "capture.h"
#include "uinput.h"

unsigned int screenwidth = 1920;
unsigned int screenheight = 1080;
const unsigned int bpp = 4;

#ifdef NC4
/* the OSD and the cursor coordinates are 1280x720 */
unsigned int nativewidth = 1280;
unsigned int nativeheight = 720;
#else
unsigned int nativewidth = 1920;
unsigned int nativeheight = 1080;
#endif

#define FBSIZE (screenwidth * screenheight * bpp)

#ifndef NC4
static gboolean server_frame_handler(gpointer data) {
	server_t* server = (server_t*) data;
   	server_update(data);
	return server->running;
}
#endif

int capture_backend_load(capture_backend_t* backend, const char* name) {
	void* handle;

	handle = dlopen(name, RTLD_NOW);

	if (handle == NULL) {
		DBG("Failed to load %s: %s", name, dlerror());
		return -1;
	}

	backend->name = name;
	backend->init = dlsym(handle, "capture_init");
	backend->execute = dlsym(handle, "capture_execute");
	backend->destroy = dlsym(handle, "capture_destroy");

	return 0;
}

int capture_backend_init(capture_backend_t* backend, uint32_t width, uint32_t height) {
	int ret;

#ifdef NC4
	if ((ret = capture_backend_load(backend, "libcapture_nc4.so")) != 0) {
		DBG("%s load failed: %d", "libcapture_nc4", ret);
	} else if ((ret = backend->init(width, height)) != 0) {
		DBG("%s init failed: %d", "libcapture_nc4", ret);
	} else {
		return 0;
	}
	WARN("No eligible capture backends found");
	return -1;
#endif

	if ((ret = capture_backend_load(backend, "libcapture_halgal.so")) != 0) {
		DBG("%s load failed: %d", "libcapture_halgal", ret);
	} else {
		if ((ret = backend->init(width, height)) != 0) {
			DBG("%s init failed: %d", "libcapture_halgal", ret);
		} else {
			return 0;
		}
	}

	if ((ret = capture_backend_load(backend, "libcapture_gm.so")) != 0) {
		DBG("%s load failed: %d", "libcapture_gm", ret);
	} else {
		if ((ret = backend->init(width, height)) != 0) {
			DBG("%s init failed: %d", "libcapture_gm", ret);
		} else {
			return 0;
		}
	}

	WARN("No eligible capture backends found");

	return -1;
}

static void server_client_gone(rfbClientPtr cl) {
	server_t* server = (server_t*) cl->screen->screenData;

	INFO("%s [%d]: Client disconnected", cl->host, server->active_clients);

	server->active_clients -= 1;
}

static enum rfbNewClientAction server_client_incoming(rfbClientPtr cl) {
	server_t* server = (server_t*) cl->screen->screenData;
	server->active_clients += 1;
	server->full_update = true;

	cl->clientGoneHook = &server_client_gone;

	INFO("%s [%d]: New client connected", cl->host, server->active_clients);

	return RFB_CLIENT_ACCEPT;
}

static void keyevent(rfbBool down, rfbKeySym key, rfbClientPtr cl) {
	uinput_key_command(down, key);
}

static void ptrevent(int buttonMask, int x, int y, rfbClientPtr cl) {
	// fprintf(stderr, "%03d x %03d: %08x\n", x, y, buttonMask);
	ptr_abs(x * nativewidth / cl->screen->width, y * nativeheight / cl->screen->height, buttonMask);
}

int server_start(server_t* server, settings_t* settings) {
	int ret;

	if ((ret = capture_backend_init(&server->capture, settings->width, settings->height)) != 0) {
		ERR("capture_init() failed: %d", ret);
		return -2;
	}

	INFO("Using capture backend: %s", server->capture.name);

	rfbLogEnable(0);

	rfbScreenInfoPtr screen = rfbGetScreen(NULL, NULL, settings->width, settings->height, 8, 3, bpp);

	if (screen == NULL) {
		ERR("rfbGetScreen() initialization failed");
		return -3;
	}

	server->active_clients = 0;
	server->settings = settings;
	server->screen = screen;
	screen->screenData = (void*) server;

	screen->newClientHook = server_client_incoming;

	if ((ret = initialize_uinput()) != 0) {
		ERR("uinput initialization failed: %d", ret);
		return -4;
	}

	// switch red and blue channels
	int tmp = screen->serverFormat.redShift;
	screen->serverFormat.redShift = screen->serverFormat.blueShift;
	screen->serverFormat.blueShift = tmp;

	screen->kbdAddEvent = keyevent;
	screen->ptrAddEvent = ptrevent;

	int fbsize = screen->width * screen->height * bpp;
	screen->frameBuffer = malloc (fbsize);
	server->capture_buffer = malloc (fbsize);
	server->full_update = true;

	if (settings->password && strlen(settings->password)) {
		char** passwords = calloc(2, sizeof(char*));
		passwords[0] = settings->password;
		screen->authPasswdData = (void*)passwords;
		screen->passwordCheck = rfbCheckPasswordByList;
	}

	rfbInitServer(screen);

	// Run event loop in background thread
	rfbRunEventLoop(screen, -1, TRUE);

	INFO("VNC server running on %d", screen->port);

	server->running = true;

	return 0;
}

#ifndef NC4
void server_bind_gmainloop(server_t* server) {
	server->timeout_ref = g_timeout_add(1000 / server->settings->framerate, server_frame_handler, (gpointer) server);
}
#endif

#define TILE_W 64
#define TILE_H 16

/*
 * Copy the tiles that changed since the last frame to the framebuffer and mark
 * them as modified, one rectangle per row of tiles, so that an unchanged
 * screen sends nothing and a small change doesn't resend the whole frame.
 */
static void server_mark_changes(server_t* server) {
	rfbScreenInfoPtr screen = server->screen;
	uint8_t* dst = (uint8_t*) screen->frameBuffer;
	uint8_t* src = server->capture_buffer;
	int stride = screen->width * bpp;
	int tx, ty, y;

	if (server->full_update) {
		memcpy(dst, src, stride * screen->height);
		rfbMarkRectAsModified(screen, 0, 0, screen->width, screen->height);
		server->full_update = false;
		return;
	}

	for (ty = 0; ty < screen->height; ty += TILE_H) {
		int th = screen->height - ty < TILE_H ? screen->height - ty : TILE_H;
		int x1 = -1, x2 = -1;

		for (tx = 0; tx < screen->width; tx += TILE_W) {
			int tw = screen->width - tx < TILE_W ? screen->width - tx : TILE_W;
			bool changed = false;

			for (y = ty; y < ty + th; y++) {
				size_t off = (size_t) y * stride + tx * bpp;
				if (memcmp(dst + off, src + off, tw * bpp) != 0) {
					changed = true;
					break;
				}
			}
			if (!changed)
				continue;
			for (y = ty; y < ty + th; y++) {
				size_t off = (size_t) y * stride + tx * bpp;
				memcpy(dst + off, src + off, tw * bpp);
			}
			if (x1 < 0)
				x1 = tx;
			x2 = tx + tw;
		}
		if (x1 >= 0)
			rfbMarkRectAsModified(screen, x1, ty, x2, ty + th);
	}
}

int server_update(server_t* server) {
	int ret;
	if (server->active_clients > 0) {
		int fbsize = server->screen->width * server->screen->height * bpp;
		if ((ret = server->capture.execute(server->capture_buffer, fbsize)) != 0) {
			ERR("capture execute failed: %08x", ret);
			return -5;
		}

		server_mark_changes(server);
	}

	return 0;
}


int server_stop(server_t* server) {
	INFO("Shutting down...");
#ifndef NC4
	g_source_remove(server->timeout_ref);
#endif
	server->running = false;
	server->capture.destroy();
	rfbShutdownServer(server->screen, TRUE);
	free(server->screen->frameBuffer);
	free(server->capture_buffer);
	rfbScreenCleanup(server->screen);
	shutdown_uinput();

	return 0;
}
