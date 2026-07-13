/*
 * of_oom.cpp -- make allocation failure visible on hardware.
 *
 * Diablo links the toolchain's REAL libstdc++/libsupc++ (not the SDK's
 * of_cxxabi.cpp shim, which is the TU that prints "operator new: out of
 * memory" for every other repo) and compiles with -fno-exceptions. A failed
 * `operator new` therefore runs libsupc++'s __throw_bad_alloc -> terminate ->
 * abort -> musl a_crash: a bare CPU trap with NOTHING on the console. The
 * kernel's fatal_trap then flips the display to the terminal, so all a player
 * sees is the stale boot log -- indistinguishable from a wild pointer or heap
 * corruption. That ambiguity is what issue #4 (crash entering Diablo's lair)
 * got stuck on: we could not tell "out of memory" from "memory corrupted".
 *
 * Overriding the global operator new fixes that for the cost of one printf on
 * a path that already ends in abort(). The size is worth having: a failure on
 * a large request with plenty of heap left means FRAGMENTATION, while a
 * failure on a small request means genuine EXHAUSTION -- and those have
 * different fixes.
 *
 * Overriding the scalar form is enough to cover array-new too: libsupc++'s
 * operator new[] is implemented in terms of operator new. The nothrow form
 * also routes through here, so it now aborts with a message instead of
 * returning nullptr -- no regression, because with -fno-exceptions the
 * throwing path it wraps was already fatal, and nothing on this target
 * depends on getting a nullptr back. Over-aligned (align_val_t) new keeps
 * libsupc++'s definition and still fails silently; DevilutionX does not use
 * it here.
 */
#include <cstdio>
#include <cstdlib>
#include <new>

#include <unistd.h>

extern "C" {
#include "of.h"
}

namespace {

/* Report what the heap looked like at the moment of failure.
 *
 * caps->heap_size is NOT live headroom: caps_table.c computes it once at boot
 * as (mmap_bottom - heap_base). musl's mallocng serves most of this app's
 * allocations through mmap, and the kernel carves those DOWNWARD from
 * mmap_bottom, so that space disappears without moving the program break.
 * Reporting "free = heap_size - brk usage" therefore reads ~51 MB free at the
 * exact moment a 40 KB request is failing, which is worse than no number at
 * all. The probe ladder below measures what can actually still be obtained,
 * which is the number that distinguishes a genuinely exhausted arena from a
 * fragmented one. */
void ReportHeapState(std::size_t failedBytes)
{
	const struct of_capabilities *caps = of_get_caps();
	const unsigned long brkNow = (unsigned long)(uintptr_t)sbrk(0);

	std::printf("[of] OUT OF MEMORY: operator new(%lu) failed\n",
	    (unsigned long)failedBytes);

	if (caps != nullptr) {
		const unsigned long base = (unsigned long)caps->heap_base;
		const unsigned long bootArena = (unsigned long)caps->heap_size;
		std::printf("[of]   heap base=%08lx brk=%08lx brk_used=%luKB "
		            "boot_arena=%luKB (NOT live headroom: mmap eats from the top)\n",
		    base, brkNow, (brkNow > base) ? (brkNow - base) / 1024ul : 0ul,
		    bootArena / 1024ul);
	} else {
		std::printf("[of]   heap caps unavailable; brk=%08lx\n", brkNow);
	}

	/* Largest block still obtainable, by probe. Every success is freed again
	 * immediately, so this neither perturbs the failure nor leaks on the way
	 * to abort(). malloc is used directly to avoid recursing into the
	 * operator new that just failed. */
	static const unsigned long kProbe[] = {
		16ul, 1024ul, 16ul * 1024, 64ul * 1024, 256ul * 1024,
		1024ul * 1024, 4ul * 1024 * 1024, 16ul * 1024 * 1024
	};
	unsigned long largest = 0;
	std::printf("[of]   probe:");
	for (unsigned long n : kProbe) {
		void *p = std::malloc(n);
		std::printf(" %lu%c=%s", n >= 1024 ? n / 1024 : n,
		    n >= 1024 ? 'K' : 'B', p != nullptr ? "ok" : "FAIL");
		if (p != nullptr) {
			largest = n;
			std::free(p);
		}
	}
	std::printf("\n[of]   largest obtainable=%luKB vs failed request=%luKB "
	            "-> %s\n",
	    largest / 1024ul, (unsigned long)failedBytes / 1024ul,
	    largest >= failedBytes
	        ? "NOT exhaustion (a bigger block is obtainable right now)"
	        : "EXHAUSTED (arena really is out of space)");

	/* Retry the exact failing size. Hardware showed a 16 MB request
	 * succeeding moments after a 41 KB one failed, so the interesting
	 * question is no longer "how much is left" but "what is special about
	 * THIS size". If the retry fails too, the fault is deterministic for the
	 * size class rather than a transient state. */
	std::printf("[of]   retry %lu:", (unsigned long)failedBytes);
	for (int i = 0; i < 3; i++) {
		void *p = std::malloc(failedBytes);
		std::printf(" %s", p != nullptr ? "ok" : "FAIL");
		std::free(p); /* free(nullptr) is a no-op */
	}

	/* Bracket the failing size. musl's mallocng routes each size to a size
	 * class and mmaps a group per class, so a fault confined to one class
	 * shows up as a run of FAILs surrounded by ok. Sizes straddle the
	 * page-alignment boundary the kernel's sys_mmap2 rounds to. */
	const unsigned long f = (unsigned long)failedBytes;
	const unsigned long bracket[] = {
		f / 4, f / 2, (f > 4096 ? f - 4096 : 1), f + 4096, f * 2, f * 4
	};
	std::printf("\n[of]   bracket:");
	for (unsigned long n : bracket) {
		void *p = std::malloc(n);
		std::printf(" %lu=%s", n, p != nullptr ? "ok" : "FAIL");
		std::free(p);
	}
	std::printf("\n");

	/* Push it out before abort() traps -- the terminal's UART mirror is
	 * buffered and fatal_trap's flush runs too late to help if the trap
	 * itself is what tears the console down. */
	std::fflush(stdout);
}

} // namespace

void *operator new(std::size_t size)
{
	/* malloc(0) may legitimately return nullptr; operator new must return a
	 * unique non-null pointer, so round up like libsupc++ does. */
	void *p = std::malloc(size != 0 ? size : 1);
	if (p == nullptr) {
		ReportHeapState(size);
		std::abort();
	}
	return p;
}

void operator delete(void *p) noexcept
{
	std::free(p);
}

void operator delete(void *p, std::size_t) noexcept
{
	std::free(p);
}
