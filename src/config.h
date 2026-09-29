#pragma once

#include <stddef.h>


#define LNCT_DEFAULT_CONTROLLER_PITCH_SENSITIVITY 0.25f
#define LNCT_DEFAULT_CONTROLLER_ZOOM_SPEED 15.0f
#define LNCT_DEFAULT_MOUSE_PITCH_SENSITIVITY 1.50f
#define LNCT_DEFAULT_INVERT_CONTROLLER_ZOOM 0

typedef struct
{
	float controller_pitch_sensitivity;
	float controller_zoom_speed;
	float mouse_pitch_sensitivity;
	int invert_controller_pitch;
	int invert_controller_zoom;
	int debug_log;
} LNCT_Config;

void LNCT_SetConfigDefaults(LNCT_Config* config);
int LNCT_LoadConfig(LNCT_Config* config, char* path, size_t path_size);
