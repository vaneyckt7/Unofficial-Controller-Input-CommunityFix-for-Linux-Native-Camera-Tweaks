#include <dlfcn.h>
#include <sys/types.h>
#include <stdatomic.h>
#include <time.h>
#include <wchar.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "camera_state.h"
#include "camera_timing.h"
#include "controller_input.h"
#include "debug_log.h"
#include "debug_wobble.h"
#include "file_stamp.h"
#include "portable_paths.h"
#include "sdl_bindings.h"
#include "stick_owner.h"
#include "utils.h"
#include "offsets.h"
#include "vtable_hook.h"


#define VERSION "1.0.22-community-input-fix"
//Tested Game build:
	//4.1.1.7398727
	//4.1.1.7209685

#define MOUSE_ZOOM_FACTOR 0.25f

#define CAMERA_OBJECT_MODE_FLAGS_OFFSET 0xA8
#define CAMERA_OBJECT_ROTATION_SPEED_OFFSET 0xC4
#define CAMERA_OBJECT_PITCH_OFFSET 0x164
#define BINDING_RELOAD_INTERVAL_NS 500000000L
#define DEBUG_REPORT_INTERVAL_NS 500000000L
#define DEBUG_REPORTED_CAMERAS 4

/* BG3 camera InputEvent, as read by the camera input handler. */
#define CAMERA_INPUT_ZOOM_IN 0x68
#define CAMERA_INPUT_ZOOM_OUT 0x69
#define CAMERA_INPUT_EVENT_VALUE_OFFSET 0x18
#define CAMERA_INPUT_HANDLER_EVENT_LOAD_OFFSET 0x40d


static pid_t g_pid = 0;
static int g_setup_succeeded;
static uint64_t g_game_build;

static float* g_roll;

static atomic_int g_mouse_delta_y;
static atomic_int g_mouse_wheel_y;
static atomic_int g_roll_keydown = 0;
static atomic_int g_roll_input_sources = 0;
static atomic_int g_mod_owns_relative_mouse_mode = 0;
static atomic_int g_ignore_next_mouse_motion = 0;
static atomic_int g_controller_right_stick_y;
static atomic_int g_controller_right_stick_axis_motion_y = 0;
static atomic_int g_controller_left_stick_button_down = 0;
static atomic_int g_controller_left_stick_used_for_zoom = 0;
static atomic_int g_controller_instance_id = -1;
static LNCT_StickOwner g_right_stick_owner;

static SDL_Event g_left_stick_down_event;
static int g_has_left_stick_down_event;
static SDL_Event g_deferred_controller_events[2];
static int g_deferred_controller_event_count;
static int g_deferred_controller_event_index;

static LNCT_Config g_config =
{
	.controller_pitch_sensitivity = LNCT_DEFAULT_CONTROLLER_PITCH_SENSITIVITY,
	.controller_zoom_speed = LNCT_DEFAULT_CONTROLLER_ZOOM_SPEED,
	.mouse_pitch_sensitivity = LNCT_DEFAULT_MOUSE_PITCH_SENSITIVITY,
	.invert_controller_pitch = 0,
	.invert_controller_zoom = LNCT_DEFAULT_INVERT_CONTROLLER_ZOOM,
};
static char g_config_path[1024];

static LNCT_CameraTiming g_camera_timings[LNCT_MAX_TRACKED_CAMERAS];
static int g_camera_timing_evictions;

/* Diagnostics (LNCT_DEBUG=1).  Camera statistics belong to the camera-hook thread. */
typedef struct
{
	void* camera_object;
	unsigned calls;
	float delta_time;
	float mod_pitch;
	float external_pitch;
	float rotation_speed;
	uint32_t mode_flags;
	float pitch;
	float zoom;
	LNCT_Wobble wobble;
} DebugCameraStats;
static struct
{
	struct timespec window_start;
	unsigned calls;
	unsigned untracked_cameras;
	DebugCameraStats cameras[DEBUG_REPORTED_CAMERAS];
} g_debug_camera;
static atomic_llong g_debug_last_camera_call_ns;
static atomic_int g_debug_right_stick_y_consumed;
static atomic_int g_debug_wheel_consumed;
static atomic_int g_debug_zoom_events;
static atomic_int g_debug_zoom_events_blocked;
static atomic_llong g_debug_zoom_wait_max_ns;
static int g_debug_right_stick_ui;
static struct timespec g_debug_input_window_start;

static BindingSet g_bs;
static const ActionBindings* g_binds[1];
static ActionBindings g_default_roll_binding;
static char g_input_config_path[1200];
static LNCT_FileStamp g_input_config_stamp;
static struct timespec g_last_binding_check;
	 
static int (*O_PollEvent)(SDL_Event*) = NULL;
static SDL_bool (*O_SDL_GetRelativeMouseMode)(void) = NULL;
static int (*O_SDL_SetRelativeMouseMode)(SDL_bool) = NULL;
static SDL_GameController* (*O_SDL_GameControllerFromInstanceID)(SDL_JoystickID) = NULL;
static Sint16 (*O_SDL_GameControllerGetAxis)(SDL_GameController*, SDL_GameControllerAxis) = NULL;
static Uint8 (*O_SDL_GameControllerGetButton)(SDL_GameController*, SDL_GameControllerButton) = NULL;

#if defined(__GLIBC__)
extern void* LNCT_DlsymCompat(void*, const char*);
__asm__(".symver LNCT_DlsymCompat,dlsym@GLIBC_2.2.5");
#else
#define LNCT_DlsymCompat dlsym
#endif

typedef float (*CalculateCameraAngle_t)(void*, uint8_t);
static CalculateCameraAngle_t O_CalculateCameraAngle;

/*
 * BG3's world-camera input handler (a virtual method).  Returns 0x0101 when it
 * handled the event, otherwise 0; the result is passed through unchanged.
 */
typedef uint64_t (*CameraInputHandler_t)(void*, void*, uint8_t*);
static CameraInputHandler_t O_CameraInputHandler;
/* Set once by Setup() on the SDL thread, which is also the only reader. */
static int g_camera_input_hooked;

enum
{
	ROLL_INPUT_MOUSE = 1,
	ROLL_INPUT_KEYBOARD = 2,
};


static void SetRollInputSource(int source, int active)
{
	if (active)
	{
		int previous_sources = atomic_fetch_or(&g_roll_input_sources, source);
		if (previous_sources != 0)
			return;

		atomic_store(&g_roll_keydown, 1);
		if (O_SDL_GetRelativeMouseMode && O_SDL_SetRelativeMouseMode
			&& O_SDL_GetRelativeMouseMode() != SDL_TRUE
			&& O_SDL_SetRelativeMouseMode(SDL_TRUE) == 0)
		{
			atomic_store(&g_mod_owns_relative_mouse_mode, 1);
			atomic_store(&g_ignore_next_mouse_motion, 1);
		}
		/* Enabling relative mode can itself create a synthetic motion event. */
		atomic_store(&g_mouse_delta_y, 0);
		return;
	}

	int previous_sources = atomic_fetch_and(&g_roll_input_sources, ~source);
	if ((previous_sources & ~source) != 0)
		return;

	atomic_store(&g_roll_keydown, 0);
	atomic_store(&g_mouse_delta_y, 0);
	atomic_store(&g_ignore_next_mouse_motion, 0);
	if (atomic_exchange(&g_mod_owns_relative_mouse_mode, 0)
		&& O_SDL_SetRelativeMouseMode)
	{
		O_SDL_SetRelativeMouseMode(SDL_FALSE);
	}
}

static void ResetInputState(void)
{
	atomic_store(&g_roll_input_sources, 0);
	atomic_store(&g_roll_keydown, 0);
	atomic_store(&g_mouse_delta_y, 0);
	atomic_store(&g_mouse_wheel_y, 0);
	atomic_store(&g_ignore_next_mouse_motion, 0);
	if (atomic_exchange(&g_mod_owns_relative_mouse_mode, 0)
		&& O_SDL_SetRelativeMouseMode)
	{
		O_SDL_SetRelativeMouseMode(SDL_FALSE);
	}
}

static void ResetControllerState(void)
{
	atomic_store(&g_controller_right_stick_y, 0);
	atomic_store(&g_controller_right_stick_axis_motion_y, 0);
	atomic_store(&g_controller_left_stick_button_down, 0);
	atomic_store(&g_controller_left_stick_used_for_zoom, 0);
	LNCT_StickOwnerReset(&g_right_stick_owner);
}

/*
 * SDL axis/button events describe transitions, not an authoritative snapshot.
 * If a release/centering event is lost during a focus or input-mode transition,
 * the old sample otherwise remains active forever.  Reconcile it with SDL's
 * live controller state on each camera update when the query API is available.
 */
static void RefreshControllerState(void)
{
	if (!O_SDL_GameControllerFromInstanceID
		|| !O_SDL_GameControllerGetAxis
		|| !O_SDL_GameControllerGetButton)
	{
		return;
	}

	SDL_JoystickID instance_id = (SDL_JoystickID)atomic_load(&g_controller_instance_id);
	if (instance_id < 0)
		return;

	SDL_GameController* controller = O_SDL_GameControllerFromInstanceID(instance_id);
	if (!controller)
		return;

	int16_t right_stick_y = O_SDL_GameControllerGetAxis(
		controller, SDL_CONTROLLER_AXIS_RIGHTY);
	int left_stick_down = O_SDL_GameControllerGetButton(
		controller, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
	int axis_active = LNCT_NormalizeControllerAxis(right_stick_y) != 0.f;

	atomic_store(&g_controller_right_stick_y, right_stick_y);
	atomic_store(&g_controller_right_stick_axis_motion_y, axis_active);
	atomic_store(&g_controller_left_stick_button_down, left_stick_down);
	if (left_stick_down && axis_active)
		atomic_store(&g_controller_left_stick_used_for_zoom, 1);
}

static void UseDefaultRollBinding(void)
{
	memset(&g_default_roll_binding, 0, sizeof(g_default_roll_binding));
	snprintf(g_default_roll_binding.name, sizeof(g_default_roll_binding.name),
		"CameraToggleMouseRotate");
	g_default_roll_binding.binding_count = 1;
	g_default_roll_binding.bindings[0].type = BINDING_MOUSE;
	g_default_roll_binding.bindings[0].scancode = SDL_SCANCODE_UNKNOWN;
	g_default_roll_binding.bindings[0].mouse_button = SDL_BUTTON_MIDDLE;
	snprintf(g_default_roll_binding.bindings[0].raw,
		sizeof(g_default_roll_binding.bindings[0].raw), "middle");
	g_binds[0] = &g_default_roll_binding;
}

static int ReloadRollBindings(void)
{
	char discovered_path[sizeof(g_input_config_path)];
	BindingSet new_bindings;
	if (LNCT_FindInputConfigPath(discovered_path, sizeof(discovered_path))
		&& LoadBindingsFromFile(discovered_path, &new_bindings))
	{
		snprintf(g_input_config_path, sizeof(g_input_config_path), "%s", discovered_path);
		LNCT_ReadFileStamp(g_input_config_path, &g_input_config_stamp);
		const ActionBindings* bindings = FindAction(&new_bindings, "CameraToggleMouseRotate");
		if (bindings && bindings->binding_count > 0)
		{
			g_bs = new_bindings;
			g_binds[0] = FindAction(&g_bs, "CameraToggleMouseRotate");
			fprintf(stdout, "\e[1;95m[LNCT]\e[0m Mouse-rotate bindings: %s\n",
				g_input_config_path);
			return 1;
		}
		UseDefaultRollBinding();
		fprintf(stdout, "\e[1;95m[LNCT]\e[0m CameraToggleMouseRotate uses BG3 default: middle mouse\n");
		return 1;
	}

	if (!g_binds[0])
	{
		UseDefaultRollBinding();
		fprintf(stderr,
			"\e[1;95m[LNCT]\e[0m WARN: BG3 input config unavailable; using middle mouse button\n");
	}
	return 0;
}

static void MaybeReloadRollBindings(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return;
	long long elapsed_ns = (long long)(now.tv_sec - g_last_binding_check.tv_sec) * 1000000000LL
		+ (now.tv_nsec - g_last_binding_check.tv_nsec);
	if (elapsed_ns >= 0 && elapsed_ns < BINDING_RELOAD_INTERVAL_NS)
		return;
	g_last_binding_check = now;

	LNCT_FileStamp current_stamp;
	if (!LNCT_ReadFileStamp(g_input_config_path, &current_stamp))
	{
		ReloadRollBindings();
		return;
	}
	if (LNCT_FileStampEqual(&current_stamp, &g_input_config_stamp))
	{
		return;
	}

	/* Reload through a temporary BindingSet. A partially written file leaves
	 * the last known-good binding active and is retried on the next poll. */
	if (ReloadRollBindings())
		ResetInputState();
}


static float GetCameraDeltaTime(void* camera_object, LNCT_CameraTiming** out_timing)
{
	*out_timing = NULL;
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0.f;

	float delta_time;
	int evicted;
	*out_timing = LNCT_UpdateCameraTiming(g_camera_timings, LNCT_MAX_TRACKED_CAMERAS,
		camera_object, &now, &delta_time, &evicted);
	if (evicted)
		g_camera_timing_evictions++;
	return delta_time;
}


static long long TimespecNanoseconds(const struct timespec* time)
{
	return (long long)time->tv_sec * 1000000000LL + time->tv_nsec;
}

static long long MonotonicNanoseconds(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return TimespecNanoseconds(&now);
}

/* Whether the map, a book or other UI has taken the right stick from the camera. */
/*
 * While UI owns the stick, L3 + stick does not zoom, so an L3 click must reach
 * BG3.  Unlike RightStickDrivesUI(), safe on the input thread: it logs nothing.
 */
static int RightStickOwnedByUI(void)
{
	return LNCT_StickOwnerIsUI(&g_right_stick_owner, MonotonicNanoseconds());
}

static int RightStickDrivesUI(void)
{
	int ui = LNCT_StickOwnerIsUI(&g_right_stick_owner, MonotonicNanoseconds());
	if (LNCT_DebugEnabled() && ui != g_debug_right_stick_ui)
	{
		g_debug_right_stick_ui = ui;
		LNCT_DebugLog("[input] right stick now drives the %s", ui ? "UI" : "camera");
	}
	return ui;
}

static DebugCameraStats* DebugCameraSlot(void* camera_object)
{
	for (int i = 0; i < DEBUG_REPORTED_CAMERAS; i++)
	{
		DebugCameraStats* stats = &g_debug_camera.cameras[i];
		if (stats->camera_object == camera_object)
			return stats;
		if (!stats->camera_object)
		{
			stats->camera_object = camera_object;
			return stats;
		}
	}
	return NULL;
}

/*
 * One line per camera object seen in the last window:
 *   dt   - summed frame time; about 0.5 for a camera updated every frame
 *   rot  - raw value at 0xC4, the game's rotationSpeed (controller pitch uses a fixed reference)
 *   mod  - pitch degrees added by this mod
 *   ext  - pitch degrees changed by something other than this mod between calls
 * followed, when any value oscillated, by a "wobble" line: the angle BG3's
 * CalculateCameraAngle returned and camera-object offsets whose direction of
 * change reversed, as reversals(min..max).
 */
static void DebugCameraReport(const struct timespec* now)
{
	if (g_debug_camera.window_start.tv_sec == 0 && g_debug_camera.window_start.tv_nsec == 0)
		g_debug_camera.window_start = *now;
	if (TimespecNanoseconds(now) - TimespecNanoseconds(&g_debug_camera.window_start) < DEBUG_REPORT_INTERVAL_NS)
		return;

	float stick = LNCT_NormalizeControllerAxis((int16_t)atomic_load(&g_controller_right_stick_y));
	LNCT_DebugLog("[cam] calls=%u cameras_untracked=%u slot_evictions=%d stick_y=%+.2f L3=%d mouse_rotate=%d",
		g_debug_camera.calls, g_debug_camera.untracked_cameras, g_camera_timing_evictions, stick,
		atomic_load(&g_controller_left_stick_button_down), atomic_load(&g_roll_keydown));
	for (int i = 0; i < DEBUG_REPORTED_CAMERAS; i++)
	{
		const DebugCameraStats* stats = &g_debug_camera.cameras[i];
		if (!stats->camera_object)
			break;
		LNCT_DebugLog("[cam]   %p calls=%u dt=%.3f rot=%.3f flags=0x%x pitch=%.2f mod=%+.2f ext=%+.2f zoom=%.2f",
			stats->camera_object, stats->calls, stats->delta_time, stats->rotation_speed,
			stats->mode_flags, stats->pitch, stats->mod_pitch, stats->external_pitch, stats->zoom);
		char wobble[512];
		if (LNCT_WobbleFormat(&stats->wobble, wobble, sizeof(wobble)))
			LNCT_DebugLog("[cam]   %p wobble %s", stats->camera_object, wobble);
	}
	memset(&g_debug_camera, 0, sizeof(g_debug_camera));
	g_debug_camera.window_start = *now;
}

static void DebugCameraUpdate(void* camera_object, LNCT_CameraTiming* timing, float delta_time,
	float pitch_before, float mod_pitch)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return;
	atomic_store(&g_debug_last_camera_call_ns, TimespecNanoseconds(&now));

	uint8_t* camera = (uint8_t*)camera_object;
	float pitch_after = *(float*)(camera + CAMERA_OBJECT_PITCH_OFFSET);
	g_debug_camera.calls++;
	DebugCameraStats* stats = DebugCameraSlot(camera_object);
	if (!stats)
		g_debug_camera.untracked_cameras++;
	else
	{
		stats->calls++;
		stats->delta_time += delta_time;
		stats->mod_pitch += mod_pitch;
		if (timing && timing->has_last_pitch)
			stats->external_pitch += pitch_before - timing->last_pitch;
		stats->rotation_speed = *(float*)(camera + CAMERA_OBJECT_ROTATION_SPEED_OFFSET);
		stats->mode_flags = *(uint32_t*)(camera + CAMERA_OBJECT_MODE_FLAGS_OFFSET);
		stats->pitch = pitch_after;
		stats->zoom = *(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_B_OFFSET);
		LNCT_WobbleSampleObject(&stats->wobble, camera);
	}
	if (timing)
	{
		timing->last_pitch = pitch_after;
		timing->has_last_pitch = 1;
	}
	DebugCameraReport(&now);
}

/* Tracks the angle BG3 computed from the camera, for the wobble report. */
static void DebugCameraAngle(void* camera_object, float angle)
{
	DebugCameraStats* stats = DebugCameraSlot(camera_object);
	if (stats)
		LNCT_WobbleSample(&stats->wobble.angle, angle);
}

/* Apply mod zoom and pitch input.  Returns the pitch change applied, in degrees. */
static float ApplyCameraInput(void* pCameraObject, float delta_time)
{
	int roll_keydown = atomic_load(&g_roll_keydown);
	int left_stick_button_down = atomic_load(&g_controller_left_stick_button_down);
	int right_stick_axis_motion_y = atomic_load(&g_controller_right_stick_axis_motion_y)
		&& !RightStickDrivesUI();
	if (!roll_keydown && left_stick_button_down && right_stick_axis_motion_y)
	{
		int16_t right_stick_y = (int16_t)atomic_load(&g_controller_right_stick_y);
		LNCT_ApplyZoomDelta(pCameraObject,
			LNCT_ControllerZoomDelta(right_stick_y, delta_time, g_config.controller_zoom_speed)
				* (g_config.invert_controller_zoom ? -1.f : 1.f));
		atomic_store(&g_controller_left_stick_used_for_zoom, 1);
		return 0.f;
	}
	else
	{
		int zoom = atomic_exchange(&g_mouse_wheel_y, 0);
		/* A zero delta must not overwrite BG3's in-progress zoom interpolation. */
		if (zoom != 0)
			LNCT_ApplyZoomDelta(pCameraObject, -((float)zoom * MOUSE_ZOOM_FACTOR));
	}

	if (!roll_keydown && !right_stick_axis_motion_y)
		return 0.f;

	g_roll = (float*)((uint8_t*)pCameraObject + CAMERA_OBJECT_PITCH_OFFSET);
	float original_roll = *g_roll;
	float roll = original_roll;

	/* An explicitly held mouse-rotate binding wins over stale controller state. */
	if (right_stick_axis_motion_y && !roll_keydown)
	{
		int16_t value = (int16_t)atomic_load(&g_controller_right_stick_y);
		float pitch_delta = LNCT_ControllerPitchDelta(value, delta_time)
			* g_config.controller_pitch_sensitivity;
		roll += g_config.invert_controller_pitch ? -pitch_delta : pitch_delta;
	}
	else
	{
		int delta = atomic_exchange(&g_mouse_delta_y, 0);
		roll += (float)delta * g_config.mouse_pitch_sensitivity;
	}

	if (roll > 89.f)
		roll = 89.f;
	else
		roll = roll;
	if (roll < -89.f)
		roll = -89.f;
	else
		roll = roll;
	*g_roll = roll;

	return roll - original_roll;
}

float H_CalculateCameraAngle_CallSite(void* pCameraObject, uint8_t angle)
{
	if (!g_setup_succeeded)
		return O_CalculateCameraAngle(pCameraObject, angle);

	RefreshControllerState();
	LNCT_CameraTiming* timing;
	float delta_time = GetCameraDeltaTime(pCameraObject, &timing);
	int debug = LNCT_DebugEnabled();
	float pitch_before = debug ? *(float*)((uint8_t*)pCameraObject + CAMERA_OBJECT_PITCH_OFFSET) : 0.f;
	float mod_pitch = ApplyCameraInput(pCameraObject, delta_time);
	if (debug)
		DebugCameraUpdate(pCameraObject, timing, delta_time, pitch_before, mod_pitch);

	float result = O_CalculateCameraAngle(pCameraObject, angle);
	if (debug)
		DebugCameraAngle(pCameraObject, result);
	return result;
}

/*
 * `movss [rbp+0x164], xmmN` followed by `mulss xmmN, xmmM`: the store-to-load
 * patch only fixes the view when the register BG3 stores is the one it goes on
 * to convert to radians for the orientation.
 */
static int IsExpectedPitchStore(const uint8_t* instruction)
{
	return instruction[0] == 0xF3 && instruction[1] == 0x0F
		&& instruction[2] == 0x11 && (instruction[3] & 0xC7) == 0x85
		&& instruction[4] == 0x64 && instruction[5] == 0x01
		&& instruction[6] == 0x00 && instruction[7] == 0x00
		&& instruction[8] == 0xF3 && instruction[9] == 0x0F && instruction[10] == 0x59
		&& (instruction[11] & 0xC0) == 0xC0
		&& ((instruction[11] >> 3) & 7) == ((instruction[3] >> 3) & 7);
}

/*
 * BG3 steps its pitch toward its own zoom-based target, stores it here and
 * builds the camera's orientation from the stepped value still in the
 * register.  Storing nothing kept the mod's pitch, but the view was still
 * built from pitch + step, and the step scales with frame time: the camera
 * wobbled up and down by a degree or two whenever frame times varied.
 * Loading the pitch instead of storing it makes the view use the mod's pitch
 * exactly: `movss [rbp+0x164], xmmN` (F3 0F 11) becomes `movss xmmN,
 * [rbp+0x164]` (F3 0F 10), same length and operands.
 */
uint8_t PatchUpdateCamera()
{
	uint8_t* movss = (uint8_t*)GetAddresses()->roll_movss;
	if (!IsExpectedPitchStore(movss))
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Refusing unexpected pitch patch instruction\n");
		return 0;
	}
	long page_size = sysconf(_SC_PAGESIZE);
	uint64_t page_start = (uint64_t)movss & ~(page_size - 1);
	size_t num_pages = (((uint64_t)movss + 8 - page_start) + page_size - 1) / page_size;
	if (num_pages < 1)
		num_pages = 1;

	if (mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
	{
		fprintf(stdout, "\e[1;95m[LNCT]\e[0m ERR: PatchUpdateCamera(): roll movss mprotect() failed\n");
		return 0;
	}

	uint8_t original[8];
	memcpy(original, movss, sizeof(original));
	movss[2] = 0x10;
	if (mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_EXEC) != 0)
	{
		memcpy(movss, original, sizeof(original));
		mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_EXEC);
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: PatchUpdateCamera(): couldn't restore executable page protection\n");
		return 0;
	}

	return 1;
}

uint8_t SetupCallSitesTrampoline()
{
	size_t page_size = sysconf(_SC_PAGESIZE);

	void* callsite = (void*)GetAddresses()->CalculateCameraAngle_CallSite;
	void* trampoline = AllocNear(callsite, page_size);
	if (!trampoline)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: SetupCallSitesTrampoline(): AllocNear() failed for CalculateCameraAngle()\n");
		return 0;
	}

	if (!PatchCallSite(callsite, trampoline, H_CalculateCameraAngle_CallSite))
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: SetupCallSitesTrampoline(): PatchCallSite() failed for CalculateCameraAngle()\n");
		munmap(trampoline, page_size);
		return 0;
	}

	return 1;
}

/*
 * The right stick's vertical axis reaches BG3 so that UI such as the map and
 * books can use it.  In the world, BG3 turns it into camera zoom events; while
 * the stick drives pitch, their value is hidden from the camera handler only,
 * so the game treats them as below its threshold and does not zoom.  Their
 * arrival also shows that the camera, not UI, has the stick (stick_owner.h).
 */
static uint64_t H_CameraInputHandler(void* self, void* entity, uint8_t* event)
{
	int32_t id;
	memcpy(&id, event, sizeof(id));
	if (id != CAMERA_INPUT_ZOOM_IN && id != CAMERA_INPUT_ZOOM_OUT)
		return O_CameraInputHandler(self, entity, event);

	int debug = LNCT_DebugEnabled();
	long long waited = LNCT_StickOwnerCameraAnswered(&g_right_stick_owner, MonotonicNanoseconds());
	if (debug)
	{
		atomic_fetch_add(&g_debug_zoom_events, 1);
		long long max = atomic_load(&g_debug_zoom_wait_max_ns);
		while (waited > max && !atomic_compare_exchange_weak(&g_debug_zoom_wait_max_ns, &max, waited))
			;
	}
	if (!atomic_load(&g_controller_right_stick_axis_motion_y))
		return O_CameraInputHandler(self, entity, event);

	if (debug)
		atomic_fetch_add(&g_debug_zoom_events_blocked, 1);
	float value;
	const float zero = 0.f;
	memcpy(&value, event + CAMERA_INPUT_EVENT_VALUE_OFFSET, sizeof(value));
	memcpy(event + CAMERA_INPUT_EVENT_VALUE_OFFSET, &zero, sizeof(zero));
	uint64_t result = O_CameraInputHandler(self, entity, event);
	memcpy(event + CAMERA_INPUT_EVENT_VALUE_OFFSET, &value, sizeof(value));
	return result;
}

static uint8_t HookCameraInputHandler(void)
{
	struct Sigs* sigs = GetSigs();
	uint64_t handler = PatternScanSectionUnique(sigs->CameraInputHandler, ".text");
	if (!handler)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: camera input handler pattern missing or ambiguous\n");
		return 0;
	}
	uint64_t text_size = 0;
	uint64_t text = GetSectionAddress(".text", &text_size);
	uint64_t event_load = handler + CAMERA_INPUT_HANDLER_EVENT_LOAD_OFFSET;
	if (!text || event_load + 32 > text + text_size
		|| !PatternMatchesAt((const uint8_t*)event_load, sigs->CameraInputHandler_EventLoad))
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: camera input handler event layout not recognized\n");
		return 0;
	}

	uint64_t relro_size = 0;
	uint8_t* relro = (uint8_t*)GetSectionAddress(".data.rel.ro", &relro_size);
	uint64_t* slot = relro ? LNCT_FindUniquePointerSlot(relro, relro_size, handler) : NULL;
	if (!slot)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: camera input handler vtable slot missing or ambiguous\n");
		return 0;
	}

	O_CameraInputHandler = (CameraInputHandler_t)handler;
	if (!LNCT_SwapPointerSlot(slot, (uint64_t)(uintptr_t)H_CameraInputHandler))
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: couldn't patch the camera input handler vtable slot\n");
		return 0;
	}
	GetAddresses()->CameraInputHandler = handler;
	LNCT_DebugLog("[setup] camera input handler %p hooked via vtable slot %p", (void*)handler, (void*)slot);
	return 1;
}

uint64_t FNV1a_Hash(const uint8_t* data, size_t len)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++)
    {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

uint64_t GetBuild()
{
	size_t size = 0;
	uint8_t* file = MapSelfExe(&size);
	if (!file || !size)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Couldn't read the running BG3 executable\n");
		return 0;
	}
    uint64_t hash = FNV1a_Hash(file, size);
    fprintf(stdout, "[LNCT] Binary FNV1a hash: %#lx (size: %zu)\n", hash, size);
    munmap(file, size);
	switch (hash)
	{
		case 0x59b4428151c778e5:
			return 4117209685;
		case 0x142d91e7bfe067cb:
			return 4117398727;
		default:
			return 0;
	}
}


void Setup()
{
	g_pid = getpid();
	g_setup_succeeded = 0;

	fprintf(stdout, "\e[1;95m[LNCT]\e[0m \e[1;37mLinux Native Camera Tweaks %s\e[0m\n", VERSION);
	fprintf(stdout, "\e[1;95m[LNCT]\e[0m Bug(s) ? Suggestion(s) ? Add me on discord: biiinks78\n");
	g_game_build = GetBuild();
	if (g_game_build)
		fprintf(stdout, "\e[1;95m[LNCT]\e[0m Known BG3 build detected: %lu\n", g_game_build);
	else
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: Unknown BG3 build; validating patterns and patch instructions\n");
	if (LNCT_LoadConfig(&g_config, g_config_path, sizeof(g_config_path)))
	{
		fprintf(stdout,
			"\e[1;95m[LNCT]\e[0m Config: %s (controller pitch %.3f, controller zoom %.3f, mouse pitch %.3f, pitch inverted %s, zoom inverted %s)\n",
			g_config_path,
			g_config.controller_pitch_sensitivity,
			g_config.controller_zoom_speed,
			g_config.mouse_pitch_sensitivity,
			g_config.invert_controller_pitch ? "yes" : "no",
			g_config.invert_controller_zoom ? "yes" : "no");
	}
	else
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: Couldn't load or create controller config; using defaults\n");
	}

	ReloadRollBindings();

	char debug_log_path[1024];
	if (LNCT_DebugInit(g_config.debug_log, debug_log_path, sizeof(debug_log_path)))
	{
		fprintf(stdout, "\e[1;95m[LNCT]\e[0m Debug log: %s\n", debug_log_path);
		LNCT_DebugLog("[setup] LNCT %s, build %lu, controller pitch %.3f, zoom %.3f, mouse pitch %.3f",
			VERSION, g_game_build, g_config.controller_pitch_sensitivity,
			g_config.controller_zoom_speed, g_config.mouse_pitch_sensitivity);
	}

	struct Sigs* sigs = GetSigs();
	struct Addresses* addresses = GetAddresses();
	uint64_t camera_pattern = PatternScanSectionUnique(sigs->CalculateCameraAngle_Callsite, ".text");
	if (!camera_pattern)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): camera pattern missing or ambiguous\n");
		return;
	}
	addresses->CalculateCameraAngle_CallSite = camera_pattern + 39;
	addresses->roll_movss = PatternScanSectionUnique(sigs->roll_movss, ".text");
	if (!addresses->roll_movss)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): pitch pattern missing or ambiguous\n");
		return;
	}

	O_CalculateCameraAngle = ResolveCallTarget((void*)(GetAddresses()->CalculateCameraAngle_CallSite));
	if (!O_CalculateCameraAngle)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Couldn't resolve the original BG3 camera function\n");
		return;
	}

	/* Install the guarded call-site first. Until setup succeeds it is a pure
	 * pass-through, so a later pitch-patch failure leaves BG3 behavior intact. */
	if (!SetupCallSitesTrampoline())
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): SetupCallSitesTrampoline() failed\n");
		return;
	}

	if (!PatchUpdateCamera())
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): PatchUpdateCamera() failed\n");
		return;
	}

	/* Optional: without it the right stick's vertical axis stays hidden from BG3. */
	g_camera_input_hooked = HookCameraInputHandler();
	if (!g_camera_input_hooked)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: right-stick map zoom and book scrolling stay disabled\n");
		LNCT_DebugLog("[setup] camera input handler not hooked; right stick Y hidden from BG3");
	}

	g_setup_succeeded = 1;
	LNCT_DebugLog("[setup] hooks installed");
	fprintf(stdout, "\e[1;95m[LNCT]\e[0m \e[1;92mEverything has been initialized correctly, enjoy ;)\e[0m\n");
}

void ResolveSDLSym()
{
	O_PollEvent = (int(*)(SDL_Event*))(intptr_t)LNCT_DlsymCompat(RTLD_NEXT, "SDL_PollEvent");
	O_SDL_GetRelativeMouseMode = (SDL_bool(*)(void))(intptr_t)LNCT_DlsymCompat(RTLD_NEXT, "SDL_GetRelativeMouseMode");
	O_SDL_SetRelativeMouseMode = (int(*)(SDL_bool))(intptr_t)LNCT_DlsymCompat(RTLD_NEXT, "SDL_SetRelativeMouseMode");
	O_SDL_GameControllerFromInstanceID = (SDL_GameController*(*)(SDL_JoystickID))(intptr_t)
		LNCT_DlsymCompat(RTLD_NEXT, "SDL_GameControllerFromInstanceID");
	O_SDL_GameControllerGetAxis = (Sint16(*)(SDL_GameController*, SDL_GameControllerAxis))(intptr_t)
		LNCT_DlsymCompat(RTLD_NEXT, "SDL_GameControllerGetAxis");
	O_SDL_GameControllerGetButton = (Uint8(*)(SDL_GameController*, SDL_GameControllerButton))(intptr_t)
		LNCT_DlsymCompat(RTLD_NEXT, "SDL_GameControllerGetButton");
}

/* Return non-zero when the event is handled by the mod and hidden from BG3. */
static int HandleSDLEvent(const SDL_Event* event)
{
	if (event->type == SDL_MOUSEMOTION)
	{
		if (atomic_exchange(&g_ignore_next_mouse_motion, 0))
			return 0;
		if (atomic_load(&g_roll_keydown))
			atomic_fetch_add(&g_mouse_delta_y, event->motion.yrel);
	}
	else if (event->type == SDL_MOUSEWHEEL)
	{
		atomic_fetch_add(&g_mouse_wheel_y, event->wheel.y);
		if (LNCT_DebugEnabled())
			atomic_fetch_add(&g_debug_wheel_consumed, 1);
		return 1;
	}
	else if (event->type == SDL_MOUSEBUTTONDOWN)
	{
		if (g_binds[0])
		{
			for (int i = 0; i < g_binds[0]->binding_count; i++)
			{
				const Binding* b = &g_binds[0]->bindings[i];
				if ((b->type == BINDING_MOUSE) && event->button.button == b->mouse_button)
					SetRollInputSource(ROLL_INPUT_MOUSE, 1);
			}
		}
	}
	else if (event->type == SDL_MOUSEBUTTONUP)
	{
		if (g_binds[0])
		{
			for (int i = 0; i < g_binds[0]->binding_count; i++)
			{
				const Binding* b = &g_binds[0]->bindings[i];
				if ((b->type == BINDING_MOUSE) && event->button.button == b->mouse_button)
					SetRollInputSource(ROLL_INPUT_MOUSE, 0);
			}
		}
	}
	else if (event->type == SDL_KEYDOWN)
	{
		if (g_binds[0])
		{
			for (int i = 0; i < g_binds[0]->binding_count; i++)
			{
				const Binding* b = &g_binds[0]->bindings[i];
				if ((b->type == BINDING_KEY) && event->key.keysym.scancode == b->scancode)
					SetRollInputSource(ROLL_INPUT_KEYBOARD, 1);
			}
		}
	}
	else if (event->type == SDL_KEYUP)
	{
		if (g_binds[0])
		{
			for (int i = 0; i < g_binds[0]->binding_count; i++)
			{
				const Binding* b = &g_binds[0]->bindings[i];
				if ((b->type == BINDING_KEY) && event->key.keysym.scancode == b->scancode)
					SetRollInputSource(ROLL_INPUT_KEYBOARD, 0);
			}
		}
	}
	else if ((event->type == SDL_CONTROLLERAXISMOTION) && event->caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY)
	{
		int16_t value = event->caxis.value;
		int axis_active = LNCT_NormalizeControllerAxis(value) != 0.f;
		atomic_store(&g_controller_instance_id, event->caxis.which);
		atomic_store(&g_controller_right_stick_y, value);
		atomic_store(&g_controller_right_stick_axis_motion_y, axis_active);
		if (axis_active && atomic_load(&g_controller_left_stick_button_down) && !RightStickOwnedByUI())
			atomic_store(&g_controller_left_stick_used_for_zoom, 1);
		if (LNCT_DebugEnabled())
			atomic_fetch_add(&g_debug_right_stick_y_consumed, 1);
		/* With the camera handler hooked, BG3 may use the axis for UI. */
		if (!g_camera_input_hooked)
			return 1;
		if (axis_active)
			LNCT_StickOwnerEventPassed(&g_right_stick_owner, MonotonicNanoseconds());
		return 0;
	}
	else if ((event->type == SDL_CONTROLLERBUTTONDOWN) && event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK)
	{
		atomic_store(&g_controller_instance_id, event->cbutton.which);
		atomic_store(&g_controller_left_stick_button_down, 1);
		atomic_store(&g_controller_left_stick_used_for_zoom,
			atomic_load(&g_controller_right_stick_axis_motion_y) && !RightStickOwnedByUI());
		g_left_stick_down_event = *event;
		g_has_left_stick_down_event = 1;
		return 1;
	}
	else if ((event->type == SDL_CONTROLLERBUTTONUP) && event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK)
	{
		atomic_store(&g_controller_instance_id, event->cbutton.which);
		int used_for_zoom = atomic_exchange(&g_controller_left_stick_used_for_zoom, 0);
		atomic_store(&g_controller_left_stick_button_down, 0);
		if (!used_for_zoom && g_has_left_stick_down_event)
		{
			g_deferred_controller_events[0] = g_left_stick_down_event;
			g_deferred_controller_events[1] = *event;
			g_deferred_controller_event_count = 2;
			g_deferred_controller_event_index = 0;
		}
		g_has_left_stick_down_event = 0;
		return 1;
	}
	else if (event->type == SDL_CONTROLLERDEVICEREMOVED
		|| (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_FOCUS_LOST))
	{
		ResetControllerState();
		atomic_store(&g_controller_instance_id, -1);
		ResetInputState();
		g_has_left_stick_down_event = 0;
		g_deferred_controller_event_count = 0;
		g_deferred_controller_event_index = 0;
	}

	return 0;
}

/*
 * Reports right-stick-Y events (hidden from BG3 unless the camera handler is
 * hooked), hidden mouse-wheel events, and camera zoom events that reached BG3's
 * camera handler and how many of them the mod blocked.  zoom_wait_max_ms is the
 * longest a deflected stick event waited for its zoom event; the UI takes the
 * stick after LNCT_STICK_UI_TIMEOUT_NS.  camera_idle_ms shows whether the world
 * camera was still updating at that time.
 */
static void DebugInputReport(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return;
	if (g_debug_input_window_start.tv_sec == 0 && g_debug_input_window_start.tv_nsec == 0)
		g_debug_input_window_start = now;
	if (TimespecNanoseconds(&now) - TimespecNanoseconds(&g_debug_input_window_start) < DEBUG_REPORT_INTERVAL_NS)
		return;
	g_debug_input_window_start = now;

	int right_stick_y = atomic_exchange(&g_debug_right_stick_y_consumed, 0);
	int wheel = atomic_exchange(&g_debug_wheel_consumed, 0);
	int zoom_events = atomic_exchange(&g_debug_zoom_events, 0);
	int zoom_blocked = atomic_exchange(&g_debug_zoom_events_blocked, 0);
	long long zoom_wait_max = atomic_exchange(&g_debug_zoom_wait_max_ns, 0);
	if (!right_stick_y && !wheel && !zoom_events)
		return;
	long long last_camera_call = atomic_load(&g_debug_last_camera_call_ns);
	long long camera_idle_ms = last_camera_call
		? (TimespecNanoseconds(&now) - last_camera_call) / 1000000LL : -1;
	LNCT_DebugLog("[input] right_stick_y=%d (%s) wheel_consumed=%d camera_zoom_events=%d blocked=%d zoom_wait_max_ms=%.1f camera_idle_ms=%lld",
		right_stick_y, g_camera_input_hooked ? "passed" : "consumed", wheel, zoom_events, zoom_blocked,
		(double)zoom_wait_max / 1000000.0, camera_idle_ms);
}

static int PopDeferredControllerEvent(SDL_Event* event)
{
	if (g_deferred_controller_event_index >= g_deferred_controller_event_count)
		return 0;

	*event = g_deferred_controller_events[g_deferred_controller_event_index++];
	if (g_deferred_controller_event_index >= g_deferred_controller_event_count)
	{
		g_deferred_controller_event_count = 0;
		g_deferred_controller_event_index = 0;
	}
	return 1;
}

int SDL_PollEvent(SDL_Event* event)
{
	if (!g_pid)
		Setup();

	if (!O_PollEvent)
		ResolveSDLSym();
	if (!O_PollEvent)
		return 0;
	if (!g_setup_succeeded)
		return O_PollEvent(event);
	MaybeReloadRollBindings();
	if (LNCT_DebugEnabled())
		DebugInputReport();

	/* Preserve SDL_PollEvent(NULL)'s queue-check semantics. */
	if (!event)
	{
		if (g_deferred_controller_event_index < g_deferred_controller_event_count)
			return 1;
		return O_PollEvent(NULL);
	}
	if (PopDeferredControllerEvent(event))
		return 1;

	int ret;
	while ((ret = O_PollEvent(event)) != 0)
	{
		/*
		 * Never return 0 merely because the mod consumed one event: BG3 uses
		 * SDL's conventional while(SDL_PollEvent(...)) loop, where 0 means the
		 * entire queue is empty.  Continue to the next queued event instead.
		 */
		if (HandleSDLEvent(event))
		{
			if (PopDeferredControllerEvent(event))
				return 1;
			continue;
		}
		return ret;
	}

	return ret;
}
