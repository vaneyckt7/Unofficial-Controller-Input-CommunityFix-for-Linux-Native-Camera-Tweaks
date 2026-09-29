#pragma once

#include <stddef.h>
#include <time.h>


#define LNCT_MAX_CAMERA_DELTA_TIME 0.1f
/*
 * More cameras updating each frame than slots would make LRU evict every one of
 * them before its next update, so all would get zero delta time.  Gameplay logs
 * show one live camera (up to five while loading); keep a wide margin.
 */
#define LNCT_MAX_TRACKED_CAMERAS 64

typedef struct
{
	void* camera_object;
	struct timespec last_update;
	/* Diagnostics only: pitch observed when the mod last finished with this camera. */
	float last_pitch;
	int has_last_pitch;
} LNCT_CameraTiming;

static inline float LNCT_TimespecSeconds(const struct timespec* later, const struct timespec* earlier)
{
	return (float)(later->tv_sec - earlier->tv_sec)
		+ (float)(later->tv_nsec - earlier->tv_nsec) / 1000000000.f;
}

/*
 * Return the timing slot for camera_object and store its elapsed time in
 * *out_delta_time.  Camera objects are never announced as destroyed, so a full
 * table evicts the least recently updated slot.  Always reusing one fixed slot
 * instead would make two new cameras overwrite each other on every call and
 * both would permanently receive a zero delta time.
 */
static inline LNCT_CameraTiming* LNCT_UpdateCameraTiming(LNCT_CameraTiming* timings, size_t count,
	void* camera_object, const struct timespec* now, float* out_delta_time, int* out_evicted)
{
	LNCT_CameraTiming* free_slot = NULL;
	LNCT_CameraTiming* oldest_slot = NULL;
	*out_delta_time = 0.f;
	*out_evicted = 0;

	for (size_t i = 0; i < count; i++)
	{
		LNCT_CameraTiming* timing = &timings[i];
		if (!timing->camera_object)
		{
			if (!free_slot)
				free_slot = timing;
			continue;
		}
		if (timing->camera_object != camera_object)
		{
			if (!oldest_slot || LNCT_TimespecSeconds(&timing->last_update, &oldest_slot->last_update) < 0.f)
				oldest_slot = timing;
			continue;
		}

		float delta_time = LNCT_TimespecSeconds(now, &timing->last_update);
		timing->last_update = *now;

		if (delta_time < 0.f)
			delta_time = 0.f;
		else if (delta_time > LNCT_MAX_CAMERA_DELTA_TIME)
			delta_time = LNCT_MAX_CAMERA_DELTA_TIME;
		*out_delta_time = delta_time;
		return timing;
	}

	/* A new camera gets a timing baseline; movement starts on its next update. */
	LNCT_CameraTiming* timing = free_slot ? free_slot : oldest_slot;
	if (!timing)
		return NULL;
	*out_evicted = !free_slot;
	timing->camera_object = camera_object;
	timing->last_update = *now;
	timing->has_last_pitch = 0;
	return timing;
}
