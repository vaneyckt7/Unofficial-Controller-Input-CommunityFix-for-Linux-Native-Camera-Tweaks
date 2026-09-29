#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>


#define LNCT_CONFIG_FILENAME "bg3-native-camera-tweaks.conf"

static char* Trim(char* text)
{
	while (isspace((unsigned char)*text))
		text++;

	char* end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return text;
}

static int GetConfigPath(char* path, size_t path_size)
{
	const char* config_home = getenv("XDG_CONFIG_HOME");
	/* XDG requires absolute paths; ignore invalid relative overrides. */
	if (config_home && config_home[0] == '/')
	{
		int length = snprintf(path, path_size, "%s/%s", config_home, LNCT_CONFIG_FILENAME);
		return length > 0 && (size_t)length < path_size;
	}

	const char* home = getenv("HOME");
	if (!home || !home[0])
		return 0;
	int length = snprintf(path, path_size, "%s/.config/%s", home, LNCT_CONFIG_FILENAME);
	return length > 0 && (size_t)length < path_size;
}

void LNCT_SetConfigDefaults(LNCT_Config* config)
{
	config->controller_pitch_sensitivity = LNCT_DEFAULT_CONTROLLER_PITCH_SENSITIVITY;
	config->controller_zoom_speed = LNCT_DEFAULT_CONTROLLER_ZOOM_SPEED;
	config->mouse_pitch_sensitivity = LNCT_DEFAULT_MOUSE_PITCH_SENSITIVITY;
	config->invert_controller_pitch = 0;
	config->invert_controller_zoom = LNCT_DEFAULT_INVERT_CONTROLLER_ZOOM;
	config->debug_log = 0;
}

static int EnsureParentDirectory(const char* path)
{
	char parent[1024];
	if (strlen(path) >= sizeof(parent))
		return 0;
	strcpy(parent, path);

	char* slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	if (!parent[0])
		return 0;

	for (char* cursor = parent + 1; *cursor; cursor++)
	{
		if (*cursor != '/')
			continue;
		*cursor = '\0';
		if (mkdir(parent, 0755) != 0 && errno != EEXIST)
			return 0;
		*cursor = '/';
	}
	return mkdir(parent, 0755) == 0 || errno == EEXIST;
}

static int WriteDefaultConfig(const LNCT_Config* config, const char* path)
{
	if (!EnsureParentDirectory(path))
		return 0;
	FILE* file = fopen(path, "w");
	if (!file)
		return 0;

	fprintf(file,
		"# Linux Native Camera Tweaks controller settings\n"
		"# Restart BG3 after changing this file.\n"
		"# Full-stick tilt speed is about 47 degrees/second times this value.\n"
		"controller_pitch_sensitivity=%.2f\n"
		"controller_zoom_speed=%.2f\n"
		"# Vertical camera speed while the mouse-rotate button is held.\n"
		"mouse_pitch_sensitivity=%.2f\n"
		"invert_controller_pitch=%s\n"
		"# false: stick up zooms in; true: stick up zooms out\n"
		"invert_controller_zoom=%s\n",
		config->controller_pitch_sensitivity,
		config->controller_zoom_speed,
		config->mouse_pitch_sensitivity,
		config->invert_controller_pitch ? "true" : "false",
		config->invert_controller_zoom ? "true" : "false");
	fclose(file);
	return 1;
}

static int ParseFloat(const char* value, float min, float max, float* output)
{
	errno = 0;
	char* end = NULL;
	float parsed = strtof(value, &end);
	if (errno || end == value || *Trim(end) != '\0' || !isfinite(parsed) || parsed < min || parsed > max)
		return 0;
	*output = parsed;
	return 1;
}

static int ParseBool(const char* value, int* output)
{
	if (!strcasecmp(value, "true") || !strcmp(value, "1") || !strcasecmp(value, "yes"))
	{
		*output = 1;
		return 1;
	}
	if (!strcasecmp(value, "false") || !strcmp(value, "0") || !strcasecmp(value, "no"))
	{
		*output = 0;
		return 1;
	}
	return 0;
}

int LNCT_LoadConfig(LNCT_Config* config, char* path, size_t path_size)
{
	LNCT_SetConfigDefaults(config);
	if (!GetConfigPath(path, path_size))
		return 0;

	FILE* file = fopen(path, "r");
	if (!file)
	{
		if (!WriteDefaultConfig(config, path))
			return 0;
		file = fopen(path, "r");
		if (!file)
			return 0;
	}

	char line[256];
	int has_mouse_pitch_sensitivity = 0;
	int has_invert_controller_zoom = 0;
	while (fgets(line, sizeof(line), file))
	{
		char* content = Trim(line);
		if (!content[0] || content[0] == '#')
			continue;

		char* equals = strchr(content, '=');
		if (!equals)
			continue;
		*equals = '\0';
		char* key = Trim(content);
		char* value = Trim(equals + 1);

		if (!strcmp(key, "controller_pitch_sensitivity"))
			ParseFloat(value, 0.01f, 4.0f, &config->controller_pitch_sensitivity);
		else if (!strcmp(key, "controller_zoom_speed"))
			ParseFloat(value, 0.1f, 100.0f, &config->controller_zoom_speed);
		else if (!strcmp(key, "mouse_pitch_sensitivity"))
			has_mouse_pitch_sensitivity = ParseFloat(
				value, 0.1f, 4.0f, &config->mouse_pitch_sensitivity);
		else if (!strcmp(key, "invert_controller_pitch"))
			ParseBool(value, &config->invert_controller_pitch);
		else if (!strcmp(key, "invert_controller_zoom"))
			has_invert_controller_zoom = ParseBool(value, &config->invert_controller_zoom);
		else if (!strcmp(key, "debug_log"))
			ParseBool(value, &config->debug_log);
	}

	fclose(file);
	if (!has_mouse_pitch_sensitivity)
	{
		file = fopen(path, "a");
		if (file)
		{
			fprintf(file,
				"\n# Vertical camera speed while the mouse-rotate button is held.\n"
				"mouse_pitch_sensitivity=%.2f\n",
				config->mouse_pitch_sensitivity);
			fclose(file);
		}
	}
	if (!has_invert_controller_zoom)
	{
		file = fopen(path, "a");
		if (file)
		{
			fprintf(file,
				"\n# false: stick up zooms in; true: stick up zooms out\n"
				"invert_controller_zoom=%s\n",
				config->invert_controller_zoom ? "true" : "false");
			fclose(file);
		}
	}
	return 1;
}
