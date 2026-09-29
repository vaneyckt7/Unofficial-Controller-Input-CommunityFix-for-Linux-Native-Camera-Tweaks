#pragma once

#include <math.h>
#include <stdint.h>


/* Match BG3 Native Camera Tweaks 2.4.5 controller defaults. */
#define LNCT_CONTROLLER_DEADZONE 0.15f
#define LNCT_CONTROLLER_CAMERA_ROTATION_MULT 2.0f
#define LNCT_CONTROLLER_PITCH_MULT 0.5f

/*
 * SDL's signed axis range is asymmetric: -32768..32767.  Normalize both
 * directions to [-1, 1], remove the deadzone, then stretch the remaining
 * range back to [0, 1].  The Windows mod applies the same outer-deadzone
 * normalization before camera rotation.
 */
static inline float LNCT_NormalizeControllerAxis(int16_t value)
{
	float normalized = value < 0 ? (float)value / 32768.0f : (float)value / 32767.0f;
	float magnitude = normalized < 0.0f ? -normalized : normalized;

	if (magnitude <= LNCT_CONTROLLER_DEADZONE)
		return 0.0f;

	magnitude = (magnitude - LNCT_CONTROLLER_DEADZONE) / (1.0f - LNCT_CONTROLLER_DEADZONE);
	return normalized < 0.0f ? -magnitude : magnitude;
}

/*
 * BG3's default camera rotationSpeed (camera object +0xC4).  Some areas, such as
 * the Blighted Village, lower the live value by about 30%; the Windows mod reads
 * it every frame and slows pitch there too.  Using the default everywhere keeps
 * controller pitch speed the same in every area.
 */
#define LNCT_CONTROLLER_REFERENCE_ROTATION_SPEED 47.444f

/*
 * Windows 2.4.5, with the reference speed in place of camera.rotationSpeed:
 *   pitchDelta = normalizedAxis * ControllerCameraRotationMult
 *              * deltaTime * rotationSpeed * ControllerPitchMult
 */
static inline float LNCT_ControllerPitchDelta(int16_t value, float delta_time)
{
	return LNCT_NormalizeControllerAxis(value)
		* LNCT_CONTROLLER_CAMERA_ROTATION_MULT
		* delta_time
		* LNCT_CONTROLLER_REFERENCE_ROTATION_SPEED
		* LNCT_CONTROLLER_PITCH_MULT;
}

static inline float LNCT_ControllerZoomDelta(int16_t value, float delta_time, float zoom_speed)
{
	/* SDL reports stick-up as negative. A negative zoom delta moves closer. */
	return LNCT_NormalizeControllerAxis(value) * delta_time * zoom_speed;
}
