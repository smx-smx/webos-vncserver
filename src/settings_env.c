#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "log.h"

static void env_int(const char* name, int* value) {
	const char* s = getenv(name);
	if (s && *s) *value = atoi(s);
}

int settings_load_env(settings_t* settings) {
	const char* password = getenv("VNC_PASSWORD");

	env_int("VNC_WIDTH", &settings->width);
	env_int("VNC_HEIGHT", &settings->height);
	env_int("VNC_FRAMERATE", &settings->framerate);
	if (settings->framerate <= 0) settings->framerate = 1;
	if (password) {
		free(settings->password);
		settings->password = strdup(password);
	}

	INFO("%dx%d, %d fps, %s", settings->width, settings->height, settings->framerate,
		settings->password && *settings->password ? "password set" : "no password");
	return 0;
}
