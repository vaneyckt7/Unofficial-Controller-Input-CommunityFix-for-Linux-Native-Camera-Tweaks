#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "camera_timing.h"


#define SLOT_COUNT 8

static int NearlyEqual(float a, float b)
{
	return fabsf(a - b) < 0.0001f;
}

static void Advance(struct timespec* now, long nanoseconds)
{
	now->tv_nsec += nanoseconds;
	while (now->tv_nsec >= 1000000000L)
	{
		now->tv_nsec -= 1000000000L;
		now->tv_sec++;
	}
}

static void* Camera(uintptr_t id)
{
	return (void*)(0x10000 + id * 0x100);
}

static float Update(LNCT_CameraTiming* timings, void* camera, const struct timespec* now, int* evicted)
{
	float delta_time;
	LNCT_CameraTiming* timing = LNCT_UpdateCameraTiming(timings, SLOT_COUNT, camera, now, &delta_time, evicted);
	assert(timing && timing->camera_object == camera);
	return delta_time;
}

/* Accumulated delta time of the first camera while `camera_count` new cameras update every frame. */
static float RunFrames(LNCT_CameraTiming* timings, struct timespec* now, uintptr_t first_id,
	int camera_count, int frames)
{
	float accumulated = 0.f;
	int evicted;
	for (int frame = 0; frame < frames; frame++)
	{
		Advance(now, 16666667L);
		for (int camera = 0; camera < camera_count; camera++)
		{
			float delta_time = Update(timings, Camera(first_id + (uintptr_t)camera), now, &evicted);
			if (camera == 0)
				accumulated += delta_time;
		}
	}
	return accumulated;
}

/* Like RunFrames, but every update happens at a distinct time, as in the game. */
static float RunRoundRobin(LNCT_CameraTiming* timings, size_t slot_count, struct timespec* now,
	uintptr_t first_id, int camera_count, int frames)
{
	float accumulated = 0.f;
	int evicted;
	long per_update = 16666667L / camera_count;
	for (int frame = 0; frame < frames; frame++)
	{
		for (int camera = 0; camera < camera_count; camera++)
		{
			float delta_time;
			Advance(now, per_update);
			LNCT_UpdateCameraTiming(timings, slot_count, Camera(first_id + (uintptr_t)camera), now,
				&delta_time, &evicted);
			if (camera == 0)
				accumulated += delta_time;
		}
	}
	return accumulated;
}

int main(void)
{
	LNCT_CameraTiming timings[SLOT_COUNT];
	struct timespec now = { 100, 0 };
	int evicted;

	/* A new camera gets a baseline, then real elapsed time. */
	memset(timings, 0, sizeof(timings));
	assert(NearlyEqual(Update(timings, Camera(1), &now, &evicted), 0.f) && !evicted);
	Advance(&now, 20000000L);
	assert(NearlyEqual(Update(timings, Camera(1), &now, &evicted), 0.02f) && !evicted);

	/* Stalls are clamped and a clock step backwards never produces negative time. */
	Advance(&now, 2000000000L);
	assert(NearlyEqual(Update(timings, Camera(1), &now, &evicted), LNCT_MAX_CAMERA_DELTA_TIME));
	struct timespec earlier = { 1, 0 };
	assert(NearlyEqual(Update(timings, Camera(1), &earlier, &evicted), 0.f));

	/* A full table evicts the least recently updated camera, not always slot 0. */
	memset(timings, 0, sizeof(timings));
	now.tv_sec = 200;
	now.tv_nsec = 0;
	for (uintptr_t i = 0; i < SLOT_COUNT; i++)
	{
		Advance(&now, 1000000L);
		Update(timings, Camera(100 + i), &now, &evicted);
		assert(!evicted);
	}
	Advance(&now, 1000000L);
	Update(timings, Camera(100), &now, &evicted);  /* refresh the oldest */
	Advance(&now, 1000000L);
	Update(timings, Camera(500), &now, &evicted);
	assert(evicted);
	for (size_t i = 0; i < SLOT_COUNT; i++)
		assert(timings[i].camera_object != Camera(101));
	int found_refreshed = 0;
	for (size_t i = 0; i < SLOT_COUNT; i++)
		found_refreshed |= timings[i].camera_object == Camera(100);
	assert(found_refreshed);

	/*
	 * Regression: after earlier areas filled every slot, two new cameras updated
	 * in the same frame used to overwrite slot 0 alternately and both received
	 * zero delta time forever, freezing controller pitch and L3 zoom.
	 */
	memset(timings, 0, sizeof(timings));
	now.tv_sec = 300;
	now.tv_nsec = 0;
	RunFrames(timings, &now, 1000, SLOT_COUNT, 1);
	float accumulated = RunFrames(timings, &now, 2000, 2, 600);
	assert(accumulated > 9.9f && accumulated < 10.1f);

	/*
	 * More live cameras than slots, each updated at its own time: LRU evicts every
	 * camera just before its next update, so none ever receives delta time.  This
	 * is why the real table must be much larger than the live camera count.
	 */
	memset(timings, 0, sizeof(timings));
	assert(RunRoundRobin(timings, SLOT_COUNT, &now, 5000, SLOT_COUNT + 1, 600) == 0.f);

	LNCT_CameraTiming large[LNCT_MAX_TRACKED_CAMERAS];
	memset(large, 0, sizeof(large));
	accumulated = RunRoundRobin(large, LNCT_MAX_TRACKED_CAMERAS, &now, 6000, 32, 600);
	assert(accumulated > 9.9f && accumulated < 10.1f);

	/* Eviction also forgets diagnostic pitch history for the reused slot. */
	memset(timings, 0, sizeof(timings));
	for (uintptr_t i = 0; i < SLOT_COUNT; i++)
	{
		Advance(&now, 1000000L);
		LNCT_CameraTiming* timing = LNCT_UpdateCameraTiming(timings, SLOT_COUNT, Camera(3000 + i), &now,
			&accumulated, &evicted);
		timing->has_last_pitch = 1;
	}
	Advance(&now, 1000000L);
	LNCT_CameraTiming* reused = LNCT_UpdateCameraTiming(timings, SLOT_COUNT, Camera(4000), &now,
		&accumulated, &evicted);
	assert(evicted && !reused->has_last_pitch);
	return 0;
}
