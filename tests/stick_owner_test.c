#include <assert.h>

#include "stick_owner.h"


#define MS 1000000LL

int main(void)
{
	LNCT_StickOwner owner;
	LNCT_StickOwnerReset(&owner);
	long long t = 5000 * MS;

	/* Nothing pending: the camera owns the stick. */
	assert(!LNCT_StickOwnerIsUI(&owner, t));
	assert(LNCT_StickOwnerCameraAnswered(&owner, t) == 0);

	/* World: each event is answered within a frame, so the UI never takes over. */
	for (int i = 0; i < 100; i++)
	{
		LNCT_StickOwnerEventPassed(&owner, t);
		assert(!LNCT_StickOwnerIsUI(&owner, t + 40 * MS));
		assert(LNCT_StickOwnerCameraAnswered(&owner, t + 40 * MS) == 40 * MS);
		t += 50 * MS;
	}

	/* UI: unanswered events.  The oldest one sets the clock; later ones do not reset it. */
	LNCT_StickOwnerEventPassed(&owner, t);
	LNCT_StickOwnerEventPassed(&owner, t + 60 * MS);
	assert(!LNCT_StickOwnerIsUI(&owner, t + 100 * MS));
	assert(LNCT_StickOwnerIsUI(&owner, t + 101 * MS));
	/* It stays with the UI with no further events, e.g. while the stick is held still. */
	assert(LNCT_StickOwnerIsUI(&owner, t + 60000 * MS));

	/* The next zoom event hands it back at once. */
	assert(LNCT_StickOwnerCameraAnswered(&owner, t + 60000 * MS) == 60000 * MS);
	assert(!LNCT_StickOwnerIsUI(&owner, t + 60000 * MS));

	/* Reset (focus loss, controller removed) returns it to the camera. */
	LNCT_StickOwnerEventPassed(&owner, t);
	assert(LNCT_StickOwnerIsUI(&owner, t + 200 * MS));
	LNCT_StickOwnerReset(&owner);
	assert(!LNCT_StickOwnerIsUI(&owner, t + 200 * MS));

	/* A zero timestamp (clock failure) still counts as pending. */
	LNCT_StickOwnerEventPassed(&owner, 0);
	assert(LNCT_StickOwnerIsUI(&owner, 200 * MS));
	return 0;
}
