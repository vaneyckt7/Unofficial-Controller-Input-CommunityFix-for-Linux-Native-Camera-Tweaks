#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


/*
 * Diagnostics: finds camera-object values that oscillate.  Each tracked float
 * counts how often its direction of change reverses within a report window;
 * a value that wobbles up and down reverses on every swing.
 */
#define LNCT_WOBBLE_BYTES 0x168  /* the camera object up to and including pitch at 0x164 */
#define LNCT_WOBBLE_FLOATS (LNCT_WOBBLE_BYTES / 4)
#define LNCT_WOBBLE_MIN_REVERSALS 2
#define LNCT_WOBBLE_MIN_RANGE 0.001f
#define LNCT_WOBBLE_MAX_REPORTED 6

typedef struct
{
	float last;
	float min;
	float max;
	int8_t direction;
	uint8_t primed;
	uint16_t reversals;
} LNCT_WobbleTrack;

typedef struct
{
	LNCT_WobbleTrack fields[LNCT_WOBBLE_FLOATS];
	LNCT_WobbleTrack angle;
} LNCT_Wobble;

static inline void LNCT_WobbleSample(LNCT_WobbleTrack* track, float value)
{
	if (!isfinite(value))
		return;
	if (!track->primed)
	{
		track->last = track->min = track->max = value;
		track->primed = 1;
		return;
	}
	if (value != track->last)
	{
		int8_t direction = value > track->last ? 1 : -1;
		if (track->direction && direction != track->direction && track->reversals < UINT16_MAX)
			track->reversals++;
		track->direction = direction;
	}
	track->last = value;
	if (value < track->min)
		track->min = value;
	if (value > track->max)
		track->max = value;
}

static inline void LNCT_WobbleSampleObject(LNCT_Wobble* wobble, const uint8_t* object)
{
	for (int i = 0; i < LNCT_WOBBLE_FLOATS; i++)
	{
		float value;
		memcpy(&value, object + i * 4, sizeof(value));
		LNCT_WobbleSample(&wobble->fields[i], value);
	}
}

static inline int LNCT_WobbleOscillates(const LNCT_WobbleTrack* track)
{
	return track->reversals >= LNCT_WOBBLE_MIN_REVERSALS
		&& track->max - track->min >= LNCT_WOBBLE_MIN_RANGE;
}

/*
 * Writes "angle=..." and up to LNCT_WOBBLE_MAX_REPORTED oscillating offsets,
 * most reversals first, as "0xOFF=reversals(min..max)".  Returns the number of
 * entries written; 0 leaves `out` empty.
 */
static inline int LNCT_WobbleFormat(const LNCT_Wobble* wobble, char* out, size_t size)
{
	int written = 0;
	size_t used = 0;
	out[0] = '\0';
	if (LNCT_WobbleOscillates(&wobble->angle))
	{
		used += (size_t)snprintf(out, size, "angle=%u(%.3f..%.3f)", wobble->angle.reversals,
			wobble->angle.min, wobble->angle.max);
		written++;
	}

	uint8_t reported[LNCT_WOBBLE_FLOATS] = { 0 };
	while (written < LNCT_WOBBLE_MAX_REPORTED && used < size)
	{
		int best = -1;
		for (int i = 0; i < LNCT_WOBBLE_FLOATS; i++)
		{
			if (!reported[i] && LNCT_WobbleOscillates(&wobble->fields[i])
				&& (best < 0 || wobble->fields[i].reversals > wobble->fields[best].reversals))
			{
				best = i;
			}
		}
		if (best < 0)
			break;
		reported[best] = 1;
		const LNCT_WobbleTrack* track = &wobble->fields[best];
		used += (size_t)snprintf(out + used, size - used, "%s0x%x=%u(%.3f..%.3f)", written ? " " : "",
			best * 4, track->reversals, track->min, track->max);
		written++;
	}
	return written;
}
