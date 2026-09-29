/*
 * Unity build of src/main.c: drives the real camera input hook, including the
 * real vtable install, against a stand-in handler and read-only vtable.
 * x86-64 only; must be linked as PIE with full RELRO (see CMakeLists.txt).
 */
#include "../src/main.c"
#include <assert.h>

/*
 * A stand-in for BG3's camera input handler: same entry bytes, same event-load
 * bytes at +0x40d.  It returns the bits of the float it saw at event+0x18.
 */
__asm__(
	".text\n.globl fake_handler\n.type fake_handler,@function\n"
	"fake_handler:\n"
	".byte 0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53\n"   /* push rbp,r15,r14,r13,r12,rbx */
	".byte 0x48,0x81,0xec,0x08,0x01,0x00,0x00\n"                  /* sub rsp,0x108 */
	".byte 0x48,0x89,0x54,0x24,0x08\n"                            /* mov [rsp+8],rdx */
	".byte 0x48,0x8b,0x16\n"                                      /* mov rdx,[rsi] */
	".byte 0x31,0xc0\n"                                           /* xor eax,eax */
	".byte 0x48,0x89,0xd1\n"                                      /* mov rcx,rdx */
	".byte 0x48,0xc1,0xe9,0x36\n"                                 /* shr rcx,0x36 */
	".byte 0x83,0xf9,0x40\n"                                      /* cmp ecx,0x40 */
	"ja 1f\n"                                                     /* 0f 87 rel32 (forced below) */
	"jmp 1f\n"
	".fill 0x40d - (. - fake_handler), 1, 0xcc\n"
	".byte 0x8b,0x10,0x89,0x54,0x24,0x10,0xf3,0x0f,0x10,0x50,0x14,0xf3,0x0f,0x10,0x48,0x18,0x8a,0x40,0x1c\n"
	"1:\n"
	"mov 8(%rsp),%rax\n"
	"movl 0x18(%rax),%eax\n"
	"add $0x108,%rsp\n"
	"pop %rbx\npop %r12\npop %r13\npop %r14\npop %r15\npop %rbp\nret\n"
	".size fake_handler, . - fake_handler\n");
extern uint64_t fake_handler(void*, void*, uint8_t*);

/* PIE + RELRO puts this relocated const table in read-only .data.rel.ro, like BG3's vtable. */
static CameraInputHandler_t const g_vtable[3] = { NULL, fake_handler, NULL };

static uint64_t CallThroughVtable(uint8_t* event)
{
	static uint64_t entity = 0;
	/* Hide the table's identity so the compiler cannot fold the const load into a direct call. */
	CameraInputHandler_t const* table = g_vtable;
	__asm__("" : "+r"(table));
	CameraInputHandler_t fn = table[1];
	return fn(NULL, &entity, event);
}

static float Bits(uint64_t v) { uint32_t b = (uint32_t)v; float f; memcpy(&f, &b, 4); return f; }

static void MakeEvent(uint8_t* event, int32_t id, float value)
{
	memset(event, 0, 0x40);
	memcpy(event, &id, 4);
	memcpy(event + 0x18, &value, 4);
	event[0x1c] = 1;
}

static float FakeCalculateCameraAngle(void* camera, uint8_t angle) { (void)camera; (void)angle; return 0.f; }
static SDL_Event g_queue[4]; static int g_queue_len, g_queue_pos;
static int FakePollEvent(SDL_Event* e)
{ if (g_queue_pos >= g_queue_len) return 0; if (e) *e = g_queue[g_queue_pos++]; return 1; }

int main(void)
{
	/* The ja must be the rel32 form for the entry signature to match. */
	assert(PatternMatchesAt((const uint8_t*)fake_handler, GetSigs()->CameraInputHandler));

	uint8_t event[0x40];
	MakeEvent(event, CAMERA_INPUT_ZOOM_IN, 0.9f);
	assert(Bits(CallThroughVtable(event)) == 0.9f);   /* not hooked yet */

	assert(LNCT_DebugInit(1, NULL, 0));
	assert(HookCameraInputHandler());
	int prot;
	assert(LNCT_QueryProtection((uintptr_t)&g_vtable[1], &prot) && prot == PROT_READ);
	assert(O_CameraInputHandler == fake_handler);
	g_camera_input_hooked = 1;

	/* Stick idle: zoom passes unchanged. */
	atomic_store(&g_controller_right_stick_axis_motion_y, 0);
	assert(Bits(CallThroughVtable(event)) == 0.9f);
	/* Stick driving pitch: the handler sees 0 for zoom in/out, the event is restored. */
	atomic_store(&g_controller_right_stick_axis_motion_y, 1);
	assert(Bits(CallThroughVtable(event)) == 0.f);
	float after; memcpy(&after, event + 0x18, 4); assert(after == 0.9f);
	MakeEvent(event, CAMERA_INPUT_ZOOM_OUT, -1.f);
	assert(Bits(CallThroughVtable(event)) == 0.f);
	memcpy(&after, event + 0x18, 4); assert(after == -1.f);
	/* Other camera inputs (rotate 0x6b) are untouched. */
	MakeEvent(event, 0x6b, 0.8f);
	assert(Bits(CallThroughVtable(event)) == 0.8f);
	assert(atomic_load(&g_debug_zoom_events) == 3 && atomic_load(&g_debug_zoom_events_blocked) == 2);

	/* A second install finds no slot holding the original any more. */
	assert(!HookCameraInputHandler());

	/* Pitch no longer follows the camera's rotationSpeed: village value, default rate. */
	g_setup_succeeded = 1; g_pid = 1;
	O_CalculateCameraAngle = FakeCalculateCameraAngle;
	static uint8_t camera[0x258];
	*(float*)(camera + 0xC4) = 33.339f;
	atomic_store(&g_controller_right_stick_y, 32767);
	H_CalculateCameraAngle_CallSite(camera, 0);
	struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	for (int i = 0; i < 30; i++) { struct timespec t = {0, 16666667L}; nanosleep(&t, NULL); H_CalculateCameraAngle_CallSite(camera, 0); }
	clock_gettime(CLOCK_MONOTONIC, &b);
	float secs = (float)(b.tv_sec - a.tv_sec) + (float)(b.tv_nsec - a.tv_nsec) / 1e9f;
	float rate = *(float*)(camera + 0x164) / secs;
	printf("pitch rate %.2f deg/s (expected 11.86 = 47.444 * 0.25)\n", rate);
	assert(rate > 11.3f && rate < 12.4f);
	atomic_store(&g_controller_right_stick_axis_motion_y, 0);

	/* RIGHTY reaches BG3 when hooked, is hidden when not; state is tracked either way. */
	O_PollEvent = FakePollEvent;
	SDL_Event e;
	for (int hooked = 1; hooked >= 0; hooked--)
	{
		g_camera_input_hooked = hooked;
		memset(g_queue, 0, sizeof(g_queue)); g_queue_len = 2; g_queue_pos = 0;
		g_queue[0].type = SDL_CONTROLLERAXISMOTION; g_queue[0].caxis.axis = SDL_CONTROLLER_AXIS_RIGHTY;
		g_queue[0].caxis.value = -20000;
		g_queue[1].type = SDL_USEREVENT;
		int seen_axis = 0, seen_user = 0;
		while (SDL_PollEvent(&e)) { seen_axis += e.type == SDL_CONTROLLERAXISMOTION; seen_user += e.type == SDL_USEREVENT; }
		assert(seen_axis == hooked && seen_user == 1);
		assert(atomic_load(&g_controller_right_stick_axis_motion_y) == 1);
		assert(atomic_load(&g_controller_right_stick_y) == -20000);
		atomic_store(&g_controller_right_stick_axis_motion_y, 0);
	}

	/* UI takes the stick when BG3's camera handler does not answer a deflected event. */
	g_camera_input_hooked = 1;
	LNCT_StickOwnerReset(&g_right_stick_owner);
	memset(g_queue, 0, sizeof(g_queue)); g_queue_len = 1; g_queue_pos = 0;
	g_queue[0].type = SDL_CONTROLLERAXISMOTION; g_queue[0].caxis.axis = SDL_CONTROLLER_AXIS_RIGHTY;
	g_queue[0].caxis.value = 32767;
	while (SDL_PollEvent(&e)) {}
	float* pitch = (float*)(camera + 0x164);
	struct timespec frame = {0, 20000000L}, ui_timeout = {0, 120000000L};
	nanosleep(&frame, NULL);
	float before = *pitch;
	H_CalculateCameraAngle_CallSite(camera, 0);
	assert(*pitch > before);                 /* within the timeout the camera still pitches */
	nanosleep(&ui_timeout, NULL);
	before = *pitch;
	H_CalculateCameraAngle_CallSite(camera, 0);
	nanosleep(&frame, NULL);
	H_CalculateCameraAngle_CallSite(camera, 0);
	assert(*pitch == before);                /* map or book open: no pitch */
	/* Nor does L3 + stick zoom the camera behind the UI. */
	float* zoom = (float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_B_OFFSET);
	float zoom_before = *zoom;
	atomic_store(&g_controller_left_stick_button_down, 1);
	nanosleep(&frame, NULL);
	H_CalculateCameraAngle_CallSite(camera, 0);
	assert(*zoom == zoom_before && *pitch == before);
	assert(!atomic_load(&g_controller_left_stick_used_for_zoom));
	atomic_store(&g_controller_left_stick_button_down, 0);
	/* So an L3 click made with the stick deflected still reaches BG3's UI. */
	memset(g_queue, 0, sizeof(g_queue)); g_queue_len = 2; g_queue_pos = 0;
	g_queue[0].type = SDL_CONTROLLERBUTTONDOWN; g_queue[0].cbutton.button = SDL_CONTROLLER_BUTTON_LEFTSTICK;
	g_queue[1].type = SDL_CONTROLLERBUTTONUP; g_queue[1].cbutton.button = SDL_CONTROLLER_BUTTON_LEFTSTICK;
	int l3_down = 0, l3_up = 0;
	while (SDL_PollEvent(&e))
	{
		l3_down += e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK;
		l3_up += e.type == SDL_CONTROLLERBUTTONUP && e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK;
	}
	assert(l3_down == 1 && l3_up == 1);
	/* A zoom event reaching the camera handler hands the stick back. */
	MakeEvent(event, CAMERA_INPUT_ZOOM_IN, 0.9f);
	assert(Bits(CallThroughVtable(event)) == 0.f);
	nanosleep(&frame, NULL);
	H_CalculateCameraAngle_CallSite(camera, 0);
	assert(*pitch > before);

	/* Losing window focus also hands the stick back. */
	memset(g_queue, 0, sizeof(g_queue)); g_queue_len = 1; g_queue_pos = 0;
	g_queue[0].type = SDL_CONTROLLERAXISMOTION; g_queue[0].caxis.axis = SDL_CONTROLLER_AXIS_RIGHTY;
	g_queue[0].caxis.value = 32767;
	while (SDL_PollEvent(&e)) {}
	nanosleep(&ui_timeout, NULL);
	assert(LNCT_StickOwnerIsUI(&g_right_stick_owner, MonotonicNanoseconds()));
	memset(g_queue, 0, sizeof(g_queue)); g_queue_len = 1; g_queue_pos = 0;
	g_queue[0].type = SDL_WINDOWEVENT; g_queue[0].window.event = SDL_WINDOWEVENT_FOCUS_LOST;
	while (SDL_PollEvent(&e)) {}
	assert(!LNCT_StickOwnerIsUI(&g_right_stick_owner, MonotonicNanoseconds()));

	/* The pitch store becomes a load: BG3's stepped pitch neither lands nor steers the view. */
	static const uint8_t update[] = {
		0x55,                                           /* push rbp */
		0x48, 0x89, 0xfd,                               /* mov rbp, rdi */
		0xf3, 0x0f, 0x11, 0x85, 0x64, 0x01, 0x00, 0x00, /* movss [rbp+0x164], xmm0 */
		0xf3, 0x0f, 0x59, 0xc2,                         /* mulss xmm0, xmm2 */
		0x5d, 0xc3,                                     /* pop rbp; ret */
	};
	/* Stores xmm0 but converts xmm1: loading into xmm0 would not steer the view. */
	static const uint8_t other_register[] = {
		0xf3, 0x0f, 0x11, 0x85, 0x64, 0x01, 0x00, 0x00, /* movss [rbp+0x164], xmm0 */
		0xf3, 0x0f, 0x59, 0xca,                         /* mulss xmm1, xmm2 */
	};
	uint8_t* code = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(code != MAP_FAILED);
	memcpy(code, update, sizeof(update));
	memcpy(code + 64, other_register, sizeof(other_register));
	assert(mprotect(code, 4096, PROT_READ | PROT_EXEC) == 0);
	float (*stepped_pitch_update)(void*, float, float, float) = (float (*)(void*, float, float, float))(void*)code;
	*pitch = 12.5f;
	assert(stepped_pitch_update(camera, 13.7f, 0.f, 1.f) == 13.7f && *pitch == 13.7f);   /* unpatched: BG3's step lands */
	GetAddresses()->roll_movss = (uint64_t)(code + 64);
	assert(!PatchUpdateCamera() && code[64 + 2] == 0x11);
	*pitch = 12.5f;
	GetAddresses()->roll_movss = (uint64_t)(code + 4);
	assert(PatchUpdateCamera());
	assert(stepped_pitch_update(camera, 13.7f, 0.f, 1.f) == 12.5f && *pitch == 12.5f);
	assert(LNCT_QueryProtection((uintptr_t)code, &prot) && prot == (PROT_READ | PROT_EXEC));
	assert(!PatchUpdateCamera());   /* no longer the expected store */
	printf("harness OK\n");
	return 0;
}
