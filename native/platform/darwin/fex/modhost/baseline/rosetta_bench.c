// SPDX-License-Identifier: MIT
// Baseline only; no Raven launch, guest OS, signals, or API round trips.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/sysctl.h>

extern uint64_t spike_integer(uint64_t count, uint64_t seed);
extern double spike_float(uint64_t count, double x, double a, double b);
static uint64_t ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t)) __builtin_trap();
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
int main(void) {
    int translated = 0;
    size_t size = sizeof translated;
    if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) || translated != 1) {
        fputs("This baseline must run under Rosetta\n", stderr);
        return 1;
    }
    const uint64_t n = 10000000;
    uint64_t expected_i = 7;
    double expected_f = 0.5;
    for (uint64_t i = 0; i != n; ++i) {
        expected_i = expected_i * 1664525 + 1013904223;
        expected_f = expected_f * 1.0000001 + 0.25;
    }
    puts("engine=Rosetta iterations=10000000 samples=5 serial_dependency_loops=1");
    // First calls include whatever translation Rosetta needs; not FEX warm-up.
    for (int sample = 0; sample != 5; ++sample) {
        uint64_t start = ns();
        uint64_t result_i = spike_integer(n, 7);
        uint64_t integer_ns = ns() - start;
        start = ns();
        double result_f = spike_float(n, 0.5, 1.0000001, 0.25);
        uint64_t float_ns = ns() - start;
        if (result_i != expected_i || memcmp(&result_f, &expected_f, sizeof result_f)) {
            fputs("kernel verification failed\n", stderr);
            return 1;
        }
        printf("sample=%d integer_ns=%" PRIu64 " float_ns=%" PRIu64 " integer_result=%" PRIu64 " float_result=%.17g\n",
               sample, integer_ns, float_ns, result_i, result_f);
    }
    return 0;
}
