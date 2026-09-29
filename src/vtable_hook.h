#pragma once

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>


/*
 * Returns the only 8-byte-aligned slot in [base, base + size) that holds `value`,
 * or NULL when there is none or more than one.  Swapping one of several slots
 * would hook some callers and not others.  This reads raw section memory,
 * which AddressSanitizer would otherwise flag at its redzones between globals.
 */
__attribute__((no_sanitize_address))
static inline uint64_t* LNCT_FindUniquePointerSlot(uint8_t* base, size_t size, uint64_t value)
{
	uint64_t* found = NULL;
	size_t start = (size_t)((8 - ((uintptr_t)base & 7)) & 7);
	for (size_t offset = start; offset + sizeof(uint64_t) <= size; offset += sizeof(uint64_t))
	{
		uint64_t* slot = (uint64_t*)(base + offset);
		if (*slot != value)
			continue;
		if (found)
			return NULL;
		found = slot;
	}
	return found;
}

/*
 * Parses one /proc/self/maps line.  Returns non-zero and sets `prot` when the
 * mapping contains `address`.
 */
static inline int LNCT_ParseMapsProtection(const char* line, uintptr_t address, int* prot)
{
	uintptr_t start, end;
	char perms[5];
	if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end, perms) != 3)
		return 0;
	if (address < start || address >= end)
		return 0;
	*prot = (perms[0] == 'r' ? PROT_READ : 0)
		| (perms[1] == 'w' ? PROT_WRITE : 0)
		| (perms[2] == 'x' ? PROT_EXEC : 0);
	return 1;
}

static inline int LNCT_QueryProtection(uintptr_t address, int* prot)
{
	FILE* maps = fopen("/proc/self/maps", "re");
	if (!maps)
		return 0;
	char line[512];
	int found = 0;
	while (!found && fgets(line, sizeof(line), maps))
		found = LNCT_ParseMapsProtection(line, address, prot);
	fclose(maps);
	return found;
}

/*
 * Replaces the pointer in `slot` with `replacement` and restores the page's
 * original protection (read-only for RELRO vtables).  The store is a single
 * aligned 8-byte write, so threads calling through the slot see either pointer.
 */
static inline int LNCT_SwapPointerSlot(uint64_t* slot, uint64_t replacement)
{
	int prot;
	if (!LNCT_QueryProtection((uintptr_t)slot, &prot))
		return 0;

	long page_size = sysconf(_SC_PAGESIZE);
	void* page = (void*)((uintptr_t)slot & ~((uintptr_t)page_size - 1));
	if (mprotect(page, (size_t)page_size, prot | PROT_WRITE) != 0)
		return 0;

	uint64_t original = *slot;
	__atomic_store_n(slot, replacement, __ATOMIC_RELEASE);
	if (mprotect(page, (size_t)page_size, prot) != 0)
	{
		__atomic_store_n(slot, original, __ATOMIC_RELEASE);
		mprotect(page, (size_t)page_size, prot);
		return 0;
	}
	return 1;
}
