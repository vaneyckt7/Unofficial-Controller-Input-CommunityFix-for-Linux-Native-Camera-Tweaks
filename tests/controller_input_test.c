#include <assert.h>
#include <math.h>
#include <stdint.h>

#include "controller_input.h"


static int NearlyEqual(float a, float b, float epsilon)
{
	return fabsf(a - b) <= epsilon;
}

int main(void)
{
	assert(LNCT_NormalizeControllerAxis(0) == 0.0f);
	assert(LNCT_NormalizeControllerAxis(4915) == 0.0f);
	assert(NearlyEqual(LNCT_NormalizeControllerAxis(INT16_MAX), 1.0f, 0.00001f));
	assert(NearlyEqual(LNCT_NormalizeControllerAxis(INT16_MIN), -1.0f, 0.00001f));

	/* Equal wall-clock time must produce equal movement at different FPS. */
	float pitch_60_fps = 0.0f;
	float pitch_120_fps = 0.0f;
	for (int i = 0; i < 60; i++)
		pitch_60_fps += LNCT_ControllerPitchDelta(INT16_MAX, 1.0f / 60.0f);
	for (int i = 0; i < 120; i++)
		pitch_120_fps += LNCT_ControllerPitchDelta(INT16_MAX, 1.0f / 120.0f);

	/* Full deflection for one second turns by the reference rotation speed. */
	assert(NearlyEqual(pitch_60_fps, LNCT_CONTROLLER_REFERENCE_ROTATION_SPEED, 0.001f));
	assert(NearlyEqual(pitch_60_fps, pitch_120_fps, 0.001f));
	assert(LNCT_ControllerPitchDelta(INT16_MIN, 1.0f) < 0.0f);

	float zoom_60_fps = 0.0f;
	float zoom_120_fps = 0.0f;
	for (int i = 0; i < 60; i++)
		zoom_60_fps += LNCT_ControllerZoomDelta(INT16_MAX, 1.0f / 60.0f, 15.0f);
	for (int i = 0; i < 120; i++)
		zoom_120_fps += LNCT_ControllerZoomDelta(INT16_MAX, 1.0f / 120.0f, 15.0f);
	assert(NearlyEqual(zoom_60_fps, 15.0f, 0.001f));
	assert(NearlyEqual(zoom_60_fps, zoom_120_fps, 0.001f));
	assert(LNCT_ControllerZoomDelta(INT16_MIN, 1.0f, 15.0f) < 0.0f);
	return 0;
}
