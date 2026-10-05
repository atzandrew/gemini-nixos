// SPDX-License-Identifier: GPL-2.0
/*
 * cpumhz — measure the real clock of each CPU on the Gemini (aarch64).
 *
 * The Gemini has no cpufreq driver and no usable `cycles` PMU event, so
 * the core clocks are unknown from userspace. This pins itself to one CPU
 * at a time and times a chain of DEPENDENT `add x0, x0, #1` instructions.
 * On both the Cortex-A53 and the Cortex-A72 an integer ADD has a 1-cycle
 * latency and a dependent chain cannot overlap, so instructions/second =
 * cycles/second = the core clock. Best of 7 runs of ~150 ms per CPU (an
 * interrupt or a preemption only ever makes a run slower). Expected
 * accuracy ~1 %; cross-checks the PLL register decode
 * (mt6797-dvfs-probe) independently.
 *
 * Usage:  cpumhz            all CPUs 0..15 that are online
 *         cpumhz 4 9        just CPU4 and CPU9
 * Output: one line per CPU, "cpuN <MHz> MHz"; offline CPUs are skipped.
 *
 * Freestanding (no libc): raw syscalls, so it builds anywhere with
 *   clang --target=aarch64-linux-gnu -O2 -static -nostdlib -ffreestanding \
 *         -fno-stack-protector -fuse-ld=lld -o cpumhz cpumhz.c
 * (bin/cpumhz/build.sh). The prebuilt binary is bin/cpumhz/cpumhz.
 */

typedef unsigned long u64;
typedef long s64;

#define SYS_write		64
#define SYS_exit_group		94
#define SYS_sched_setaffinity	122
#define SYS_sched_yield		124
#define SYS_clock_gettime	113
#define CLOCK_MONOTONIC_RAW	4

static inline s64 sys3(s64 n, s64 a, s64 b, s64 c)
{
	register s64 x8 asm("x8") = n;
	register s64 x0 asm("x0") = a;
	register s64 x1 asm("x1") = b;
	register s64 x2 asm("x2") = c;

	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
	return x0;
}

struct ts { s64 sec, nsec; };

static u64 now_ns(void)
{
	struct ts t;

	sys3(SYS_clock_gettime, CLOCK_MONOTONIC_RAW, (s64)&t, 0);
	return (u64)t.sec * 1000000000UL + (u64)t.nsec;
}

static void out(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	sys3(SYS_write, 1, (s64)s, p - s);
}

static char *utoa(char *p, u64 v)
{
	char tmp[24];
	int n = 0;

	do {
		tmp[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		*p++ = tmp[--n];
	return p;
}

#define ADDS_PER_ITER 256

/* iters x 256 dependent adds */
static void burn(u64 iters)
{
	asm volatile(
		"mov x0, #0\n"
		"1:\n"
		".rept 256\n"
		"add x0, x0, #1\n"
		".endr\n"
		"subs %0, %0, #1\n"
		"b.ne 1b\n"
		: "+r"(iters) : : "x0", "cc");
}

static u64 measure_khz(void)
{
	u64 iters = 2000, best = 0;
	int run;

	/* calibrate to ~150 ms */
	for (;;) {
		u64 t0 = now_ns();

		burn(iters);
		u64 dt = now_ns() - t0;

		if (dt > 20000000UL) {
			iters = iters * 150000000UL / dt;
			break;
		}
		iters *= 4;
	}
	for (run = 0; run < 7; run++) {
		u64 t0 = now_ns();

		burn(iters);
		u64 dt = now_ns() - t0;
		/* kHz = adds / ns * 1e6 */
		u64 khz = (iters * ADDS_PER_ITER * 1000000UL) / dt;

		if (khz > best)
			best = khz;
	}
	return best;
}

static int parse(const char *s, int *v)
{
	int n = 0;

	if (!*s)
		return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		n = n * 10 + (*s - '0');
	}
	*v = n;
	return 0;
}

static void one(int cpu)
{
	u64 mask = 1UL << cpu;
	char line[64], *p = line;
	u64 khz;

	if (sys3(SYS_sched_setaffinity, 0, sizeof(mask), (s64)&mask) != 0)
		return;	/* offline / no such CPU */
	sys3(SYS_sched_yield, 0, 0, 0);
	khz = measure_khz();
	*p++ = 'c'; *p++ = 'p'; *p++ = 'u';
	p = utoa(p, cpu);
	*p++ = ' ';
	p = utoa(p, (khz + 500) / 1000);
	*p++ = ' '; *p++ = 'M'; *p++ = 'H'; *p++ = 'z'; *p++ = '\n';
	*p = 0;
	out(line);
}

static void start_c(long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	int i, cpu;

	if (argc <= 1) {
		for (cpu = 0; cpu < 16; cpu++)
			one(cpu);
	} else {
		for (i = 1; i < argc; i++)
			if (parse(argv[i], &cpu) == 0 && cpu < 64)
				one(cpu);
	}
	sys3(SYS_exit_group, 0, 0, 0);
}

asm(".global _start\n"
    "_start:\n"
    "  mov x0, sp\n"
    "  bl start_c\n");

/* referenced only to keep start_c from being discarded */
void *const keep_start_c = (void *)start_c;
