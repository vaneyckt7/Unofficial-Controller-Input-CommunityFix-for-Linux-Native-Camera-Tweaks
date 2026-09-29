#include <assert.h>
#include <string.h>

#include "debug_wobble.h"


int main(void)
{
	static LNCT_Wobble wobble;
	char out[512];

	/* Steady, monotonic and non-finite values are not reported. */
	uint8_t object[LNCT_WOBBLE_BYTES];
	memset(object, 0, sizeof(object));
	for (int frame = 0; frame < 13; frame++)
	{
		float rising = (float)frame, nan = NAN;
		memcpy(object + 0x58, &rising, 4);
		memcpy(object + 0x10, &nan, 4);
		LNCT_WobbleSampleObject(&wobble, object);
		LNCT_WobbleSample(&wobble.angle, 30.f);
	}
	assert(LNCT_WobbleFormat(&wobble, out, sizeof(out)) == 0 && out[0] == '\0');

	/* Pitch wobbling up and down, and a tiny jitter below the range threshold. */
	memset(&wobble, 0, sizeof(wobble));
	for (int frame = 0; frame < 13; frame++)
	{
		float pitch = 30.f + ((frame & 1) ? 0.5f : -0.5f);
		float jitter = (frame & 1) ? 0.0001f : 0.f;
		memcpy(object + 0x164, &pitch, 4);
		memcpy(object + 0x20, &jitter, 4);
		LNCT_WobbleSampleObject(&wobble, object);
		LNCT_WobbleSample(&wobble.angle, pitch * 2.f);
	}
	assert(wobble.fields[0x164 / 4].reversals == 11);
	assert(LNCT_WobbleFormat(&wobble, out, sizeof(out)) == 2);
	assert(strcmp(out, "angle=11(59.000..61.000) 0x164=11(29.500..30.500)") == 0);

	/* The report is capped and never overflows a small buffer. */
	memset(&wobble, 0, sizeof(wobble));
	for (int frame = 0; frame < 5; frame++)
	{
		for (int i = 0; i < LNCT_WOBBLE_FLOATS; i++)
		{
			float v = (frame & 1) ? (float)i : 0.f;
			memcpy(object + i * 4, &v, 4);
		}
		LNCT_WobbleSampleObject(&wobble, object);
	}
	assert(LNCT_WobbleFormat(&wobble, out, sizeof(out)) == LNCT_WOBBLE_MAX_REPORTED);
	char small[20];
	memset(small, 'x', sizeof(small));
	LNCT_WobbleFormat(&wobble, small, 16);
	assert(strlen(small) < 16 && small[16] == 'x');
	return 0;
}
