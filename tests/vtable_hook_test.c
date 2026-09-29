#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vtable_hook.h"


#define TARGET 0x7f0012345670ULL
#define REPLACEMENT 0x7f00abcdef00ULL

int main(void)
{
	uint64_t table[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	/* None, exactly one, and more than one matching slot. */
	assert(!LNCT_FindUniquePointerSlot((uint8_t*)table, sizeof(table), TARGET));
	table[5] = TARGET;
	assert(LNCT_FindUniquePointerSlot((uint8_t*)table, sizeof(table), TARGET) == &table[5]);
	table[2] = TARGET;
	assert(!LNCT_FindUniquePointerSlot((uint8_t*)table, sizeof(table), TARGET));

	/* Unaligned section starts are skipped to the next aligned slot, and a
	 * value straddling two slots is not a match. */
	memset(table, 0, sizeof(table));
	table[3] = TARGET;
	assert(LNCT_FindUniquePointerSlot((uint8_t*)table + 3, sizeof(table) - 3, TARGET) == &table[3]);
	memset(table, 0, sizeof(table));
	uint64_t target = TARGET;
	memcpy((uint8_t*)table + 4, &target, sizeof(target));
	assert(!LNCT_FindUniquePointerSlot((uint8_t*)table, sizeof(table), TARGET));
	/* The last slot is searched; a partial trailing slot is not read. */
	memset(table, 0, sizeof(table));
	table[7] = TARGET;
	assert(LNCT_FindUniquePointerSlot((uint8_t*)table, sizeof(table), TARGET) == &table[7]);
	assert(!LNCT_FindUniquePointerSlot((uint8_t*)table, sizeof(table) - 1, TARGET));

	int prot = -1;
	const char* line = "7f0000001000-7f0000003000 r--p 00001000 fd:01 1234 /game/bg3\n";
	assert(LNCT_ParseMapsProtection(line, 0x7f0000001000, &prot) && prot == PROT_READ);
	assert(LNCT_ParseMapsProtection(line, 0x7f0000002fff, &prot));
	assert(!LNCT_ParseMapsProtection(line, 0x7f0000003000, &prot));
	assert(!LNCT_ParseMapsProtection(line, 0x7f0000000fff, &prot));
	assert(LNCT_ParseMapsProtection("1000-2000 rw-p 0 0:0 0\n", 0x1800, &prot)
		&& prot == (PROT_READ | PROT_WRITE));
	assert(LNCT_ParseMapsProtection("1000-2000 r-xp 0 0:0 0\n", 0x1800, &prot)
		&& prot == (PROT_READ | PROT_EXEC));
	assert(!LNCT_ParseMapsProtection("garbage\n", 0x1800, &prot));

	/* A real read-only page, like a RELRO vtable: swapped, then read-only again. */
	long page_size = sysconf(_SC_PAGESIZE);
	uint64_t* page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(page != MAP_FAILED);
	page[9] = TARGET;
	assert(mprotect(page, (size_t)page_size, PROT_READ) == 0);
	uint64_t* slot = LNCT_FindUniquePointerSlot((uint8_t*)page, (size_t)page_size, TARGET);
	assert(slot == &page[9]);
	assert(LNCT_SwapPointerSlot(slot, REPLACEMENT));
	assert(page[9] == REPLACEMENT);
	assert(LNCT_QueryProtection((uintptr_t)slot, &prot) && prot == PROT_READ);
	munmap(page, (size_t)page_size);
	return 0;
}
