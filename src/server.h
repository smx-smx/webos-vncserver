#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <rfb/rfb.h>
#ifndef NC4
#include <glib.h>
#endif
#include "settings.h"

typedef struct _capture_backend {
	const char* name;
	int (*init)(uint32_t width, uint32_t height);
	int (*execute)(uint8_t* target, uint32_t size);
	int (*destroy)(void);
} capture_backend_t;

typedef struct {
	capture_backend_t capture;
	rfbScreenInfoPtr screen;
	int active_clients;
	settings_t* settings;
	bool running;

#ifndef NC4
	guint timeout_ref;
#endif
} server_t;

int server_start(server_t* server, settings_t* settings);
int server_stop(server_t* server);
int server_update(server_t* server);
#ifndef NC4
void server_bind_gmainloop(server_t* server);
#endif
