#pragma once

#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <link.h>
#include <fcntl.h>


typedef struct
{
    const char* substr;
    uint64_t base;
} ModuleBaseCtx;

#define MAX_IDA_PATTERN_LENGTH 512
typedef struct
{
    uint16_t length;
    uint8_t bytes[MAX_IDA_PATTERN_LENGTH];
    char mask[MAX_IDA_PATTERN_LENGTH];
} Pattern;

static uint8_t HexCharToByte(char c)
{
    if ('0' <= c && c <= '9') return c - '0';
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    return 0;
}

static void BuildPattern(Pattern* p, const char* ida_pattern)
{
    p->length = 0;
    while (*ida_pattern && p->length < MAX_IDA_PATTERN_LENGTH)
	{
        while (*ida_pattern == ' ')
			ida_pattern++;
        if (ida_pattern[0] == '?' && ida_pattern[1] == '?')
		{
            p->bytes[p->length] = 0xAA;
            p->mask[p->length++] = '?';
            ida_pattern += 2;
            continue;
        }
        p->bytes[p->length] = (uint8_t)((HexCharToByte(ida_pattern[0]) << 4) | HexCharToByte(ida_pattern[1]));
        p->mask[p->length++] = 'x';
        ida_pattern += 2;
    }
}

static inline int ModuleBase_Callback(struct dl_phdr_info* info, size_t size, void* data)
{
    (void)size;
    ModuleBaseCtx* ctx = (ModuleBaseCtx*)data;

    uint8_t match = ctx->substr
        ? (strstr(info->dlpi_name, ctx->substr) != 0)
        : (info->dlpi_name[0] == '\0');

    if (match)
    {
        ctx->base = info->dlpi_addr;
        return 1;
    }
    return 0;
}

static uint64_t GetModuleBase(const char* substr)
{
    ModuleBaseCtx ctx = { substr, 0 };
    dl_iterate_phdr(ModuleBase_Callback, &ctx);
    return ctx.base;
}

static void* AllocNear(void* target, size_t size)
{
	size_t page_size = sysconf(_SC_PAGESIZE);
	uint64_t target_page = (uint64_t)target & ~(page_size - 1);

	for (int64_t delta = page_size; delta < 0x60000000; delta += page_size)
	{
		void* candidates[2] =
		{
			(void*)(target_page + delta),
			(void*)(target_page - delta)
		};
		for (int i = 0; i < 2; i++)
		{
			void* p = mmap(candidates[i], size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
			if (p != MAP_FAILED)
				return p;
		}
	}
	return NULL;
}

static void* ResolveCallTarget(void* callsite)
{
	uint8_t* cs = (uint8_t*)callsite;
	if (cs[0] != 0xE8)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: ResolveCallTarget(): instruction isn't a rel32 call %#x at %p\n", cs[0], callsite);
		return NULL;
	}
	int32_t orig_rel;
	memcpy(&orig_rel, cs + 1, 4);
	return (void*)(cs + 5 + orig_rel);
}

static void* PatchCallSite(void* callsite, void* trampoline, void* trampoline_target)
{
	uint8_t* cs = (uint8_t*)callsite;
	if (cs[0] != 0xE8)
	{

		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: PatchCallSite(): instruction isn't a rel32 call %#x at %p\n", cs[0], callsite);
		return NULL;
	}

	int32_t orig_rel;
	memcpy(&orig_rel, cs + 1, 4);
	void* original_target = (void*)(cs + 5 + orig_rel);

	int64_t rel_to_tramp = (int64_t)((uint8_t*)trampoline - (cs + 5));
	if (rel_to_tramp > INT32_MAX || rel_to_tramp < INT32_MIN)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: PatchCallSite(): trampoline addy isn't in range for a rel32 jmp\n");
		return NULL;
	}

	uint8_t* t = (uint8_t*)trampoline;
	size_t off = 0;

	t[off++] = 0x48;
	t[off++] = 0xB8;
	memcpy(t + off, &trampoline_target, 8); off += 8;

	t[off++] = 0xFF; t[off++] = 0xE0;

	long page_size = sysconf(_SC_PAGESIZE);
	if (mprotect(trampoline, (size_t)page_size, PROT_READ | PROT_EXEC) != 0)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: PatchCallSite(): couldn't protect trampoline as executable\n");
		return NULL;
	}

	uint64_t page_start = (uint64_t)cs & ~(page_size - 1);
	size_t num_pages = (((uint64_t)cs + 5 - page_start) + page_size - 1) / page_size;
	if (num_pages < 1)
		num_pages = 1;

	if (mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: PatchCallSite(): mprotect() failed on page %p for callsite %p\n", (void*)page_start, cs);
		return NULL;
	}

	int32_t new_rel = (int32_t)rel_to_tramp;
	memcpy(cs + 1, &new_rel, 4);

	if (mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_EXEC) != 0)
	{
		memcpy(cs + 1, &orig_rel, 4);
		mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_EXEC);
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: PatchCallSite(): couldn't restore executable page protection\n");
		return NULL;
	}
	return original_target;
}

static uint8_t* MapSelfExe(size_t* out_size)
{
    int fd = open("/proc/self/exe", O_RDONLY);
	if (fd < 0)
        return 0;

    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0)
    {
        close(fd);
        return 0;
	}

	void* map = mmap(0, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);

	if (map == MAP_FAILED)
	{
		return 0;
	}

	*out_size = (size_t)size;
	return (uint8_t*)map;
}

static uint64_t GetSectionAddress(const char* section, uint64_t* size_buffer)
{
    size_t file_size = 0;
    uint8_t* file = MapSelfExe(&file_size);
    if (!file)
        return 0;

    uint64_t base = GetModuleBase(0);

    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)file;
	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0)
    {
        munmap(file, file_size);
        return 0;
    }

    Elf64_Shdr* shdrs = (Elf64_Shdr*)(file + ehdr->e_shoff);
    Elf64_Shdr* shstrtab_hdr = &shdrs[ehdr->e_shstrndx];
    const char* shstrtab = (const char*)(file + shstrtab_hdr->sh_offset);

    uint64_t result = 0;

    for (int i = 0; i < ehdr->e_shnum; ++i)
    {
        const char* name = shstrtab + shdrs[i].sh_name;
        if (strcmp(name, section) == 0)
        {
            if (size_buffer)
                *size_buffer = shdrs[i].sh_size;
            result = base + shdrs[i].sh_addr;
            break;
        }
    }

	munmap(file, file_size);
    return result;
}

static int PatternMatchesAt(const uint8_t* address, const char* ida_pattern)
{
    Pattern pattern;
    BuildPattern(&pattern, ida_pattern);
    for (uint16_t p = 0; p < pattern.length; p++)
    {
        if (pattern.mask[p] == 'x' && address[p] != pattern.bytes[p])
            return 0;
    }
    return 1;
}

/* Patching the wrong occurrence is worse than disabling the mod. */
static uint64_t PatternScanSectionUnique(const char* ida_pattern, const char* section)
{
    Pattern pattern;
	BuildPattern(&pattern, ida_pattern);
    uint64_t section_size = 0;
    uint8_t* section_base = (uint8_t*)GetSectionAddress(section, &section_size);
    if (!section_base || section_size < pattern.length)
        return 0;

    uint64_t match = 0;
    unsigned int match_count = 0;
    for (uint64_t i = 0; i <= section_size - pattern.length; i++)
    {
        for (uint16_t p = 0; p < pattern.length; p++)
        {
        	if (pattern.mask[p] == 'x' && section_base[i + p] != pattern.bytes[p])
                goto next;
        }
        match = (uint64_t)(section_base + i);
        if (++match_count > 1)
            return 0;
        next:
        continue;
    }
	return match_count == 1 ? match : 0;
}
