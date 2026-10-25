#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include "server.h"
#ifndef NC4
#include "service.h"
#endif
#include "log.h"

#ifdef NC4
static volatile sig_atomic_t stop;

void int_handler(int dummy) {
	stop = 1;
}
#else
GMainLoop* loop;

void int_handler(int dummy) {
	INFO("SIGINT detected, stopping...");
	g_main_loop_quit(loop);
}
#endif

int main (int argc, char** argv) {
	int ret;

	log_init();

	if (getenv("VNCSERVER_DEBUG")) {
		INFO("Enabling verbose logging");
		log_set_level(Debug);
	}

	server_t server;
	server.active_clients = 0;
	server.running = false;

	settings_t settings;
#ifdef NC4
	/* no Luna service or settings file: run until SIGINT/SIGTERM */
	settings.framerate = 15;
	settings.width = 1280;
	settings.height = 720;
	settings.password = strdup("password");
	settings_load_env(&settings);

	if ((ret = server_start(&server, &settings)) != 0) {
		ERR("server_start() failed: %d", ret);
		return 1;
	}
	signal(SIGINT, int_handler);
	signal(SIGTERM, int_handler);
	while (!stop) {
		server_update(&server);
		usleep(1000000 / settings.framerate);
	}
	INFO("Signal received, stopping...");
	server_stop(&server);
	return 0;
#else
	loop = g_main_loop_new(NULL, FALSE);

	settings.framerate = 30;
	settings.width = 1920/2;
	settings.height = 1080/2;
	settings.password = strdup("password");

	INFO("Config load result: %d", settings_load_file(&settings, SETTINGS_PERSISTENCE_PATH));

	INFO("Starting up service...");

	if ((ret = service_init(loop, &server, &settings)) != 0) {
		WARN("service_init() failed: %d", ret);
	}

	if (!getenv("LS_SERVICE_NAMES")) {
		INFO("Running via CLI - starting anyway");
		server_start(&server, &settings);
		server_bind_gmainloop(&server);
		signal(SIGINT, int_handler);
	}

	// INFO("Adding timeout...");
	// g_timeout_add(1000 / 30, server_frame_handler, (gpointer) &server);

	INFO("Starting main loop...");

	g_main_loop_run(loop);

	INFO("loop finished!");
    g_main_loop_unref(loop);

	if (server.running) {
		server_stop(&server);
	}

	return 0;
#endif
}
