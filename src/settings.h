#pragma once

#include <stdbool.h>
#ifndef NC4
#include <pbnjson.h>
#endif

#define SETTINGS_PERSISTENCE_PATH "/media/developer/apps/usr/palm/services/org.webosbrew.vncserver.service/config.json"

typedef struct {
	char* backend;
	int width;
	int height;
	int framerate;
	char* password;
	bool autostart;
} settings_t;

#ifdef NC4
/* VNC_PASSWORD, VNC_WIDTH, VNC_HEIGHT, VNC_FRAMERATE */
int settings_load_env(settings_t* settings);
#else
int settings_load_json(settings_t* settings, jvalue_ref source);
int settings_save_json(settings_t* settings, jvalue_ref target);

int settings_load_file(settings_t* settings, char* source);
int settings_save_file(settings_t* settings, char* target);
#endif
