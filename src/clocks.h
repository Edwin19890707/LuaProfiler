#pragma once

#include <stdint.h>

#ifdef USE_RDTSCP
static inline uint64_t GetTime(void) {
	uint32_t eax, edx, aux;
	__asm__ __volatile__("rdtscp" : "=a"(eax), "=d"(edx), "=c"(aux));
	return ((uint64_t)edx) << 32 | eax;
}
#elif USE_RDTSC
static inline uint64_t GetTime(void) {
	uint32_t eax, edx;
	/*
	 * the lfence is to wait (on Intel:CPUS) until all previous
	 * instructions have been executed
	 */
	__asm__ __volatile__("lfence;rdtsc" : "=a"(eax), "=d"(edx));
	return ((uint64_t)edx) << 32 | eax;
}
#else
#include <time.h>
static inline uint64_t GetTime(void) {
	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);
	return (uint64_t)ti.tv_sec * 1000000000L + ti.tv_nsec;
}
#endif