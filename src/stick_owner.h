#pragma once

#include <stdatomic.h>


/*
 * Decides whether the right stick's vertical axis currently drives the world
 * camera or BG3's UI (map, books, other menus).
 *
 * In the world, BG3 turns every deflected right-stick-Y event into a zoom event
 * for its camera input handler, within a frame.  While UI has focus, the
 * handler receives none.  So once a deflected event has gone unanswered by the
 * camera handler for longer than the timeout, the UI owns the stick; the next
 * zoom event the handler receives hands it back to the camera.
 *
 * The input thread reports passed events, the handler reports zoom events and
 * the camera thread asks for the owner, so all state is one atomic timestamp.
 */
#define LNCT_STICK_UI_TIMEOUT_NS 100000000LL

typedef struct
{
	/* Time of the oldest deflected event not yet answered by a zoom event; 0 = none. */
	atomic_llong unanswered_since_ns;
} LNCT_StickOwner;

static inline void LNCT_StickOwnerReset(LNCT_StickOwner* owner)
{
	atomic_store(&owner->unanswered_since_ns, 0);
}

/* A deflected right-stick-Y event was passed to BG3. */
static inline void LNCT_StickOwnerEventPassed(LNCT_StickOwner* owner, long long now_ns)
{
	long long none = 0;
	atomic_compare_exchange_strong(&owner->unanswered_since_ns, &none, now_ns ? now_ns : 1);
}

/*
 * BG3's camera handler received a zoom event, so the world camera has focus.
 * Returns how long the oldest unanswered event waited, or 0 if none was waiting.
 */
static inline long long LNCT_StickOwnerCameraAnswered(LNCT_StickOwner* owner, long long now_ns)
{
	long long since = atomic_exchange(&owner->unanswered_since_ns, 0);
	return since ? now_ns - since : 0;
}

static inline int LNCT_StickOwnerIsUI(LNCT_StickOwner* owner, long long now_ns)
{
	long long since = atomic_load(&owner->unanswered_since_ns);
	return since && now_ns - since > LNCT_STICK_UI_TIMEOUT_NS;
}
