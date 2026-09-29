#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"


int main(void)
{
	LNCT_Config config;
	char path[1024];
	assert(LNCT_LoadConfig(&config, path, sizeof(path)));
	assert(fabsf(config.controller_pitch_sensitivity - 0.25f) < 0.0001f);
	assert(fabsf(config.controller_zoom_speed - 15.0f) < 0.0001f);
	assert(fabsf(config.mouse_pitch_sensitivity - 1.50f) < 0.0001f);
	assert(config.invert_controller_pitch == 0);
	assert(config.invert_controller_zoom == 0);
	assert(config.debug_log == 0);

	FILE* file = fopen(path, "w");
	assert(file);
	fprintf(file,
		"controller_pitch_sensitivity=0.18\n"
		"controller_zoom_speed=12.5\n"
		"invert_controller_pitch=true\n");
	fclose(file);

	assert(LNCT_LoadConfig(&config, path, sizeof(path)));
	assert(fabsf(config.controller_pitch_sensitivity - 0.18f) < 0.0001f);
	assert(fabsf(config.controller_zoom_speed - 12.5f) < 0.0001f);
	assert(fabsf(config.mouse_pitch_sensitivity - 1.50f) < 0.0001f);
	assert(config.invert_controller_pitch == 1);
	assert(config.invert_controller_zoom == 0);

	file = fopen(path, "r");
	assert(file);
	char content[1024] = {0};
	assert(fread(content, 1, sizeof(content) - 1, file) > 0);
	fclose(file);
	assert(strstr(content, "mouse_pitch_sensitivity=1.50"));
	/* A user's existing value survives the default changing. */
	assert(strstr(content, "controller_pitch_sensitivity=0.18"));
	assert(!strstr(content, "controller_pitch_sensitivity=0.25"));
	assert(strstr(content, "invert_controller_zoom=false"));

	file = fopen(path, "a");
	assert(file);
	fprintf(file,
		"mouse_pitch_sensitivity=1.25\n"
		"invert_controller_zoom=true\n");
	fclose(file);

	assert(LNCT_LoadConfig(&config, path, sizeof(path)));
	assert(fabsf(config.mouse_pitch_sensitivity - 1.25f) < 0.0001f);
	assert(config.invert_controller_pitch == 1);
	assert(config.invert_controller_zoom == 1);
	assert(config.debug_log == 0);

	/* Diagnostics are opt-in and never written to the default file. */
	assert(!strstr(content, "debug_log"));
	file = fopen(path, "a");
	assert(file);
	fprintf(file, "debug_log=true\n");
	fclose(file);
	assert(LNCT_LoadConfig(&config, path, sizeof(path)));
	assert(config.debug_log == 1);
	return 0;
}
