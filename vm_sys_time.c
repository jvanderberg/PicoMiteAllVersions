/*
 * VM syscall conversion rule:
 * - copy/adapt legacy implementation code as closely as possible
 * - copy/adapt dependent legacy helpers too when needed
 * - do not invent new algorithms when legacy code already exists
 * - do not call, wrap, or dispatch back into legacy handlers
 * Any deviation from legacy implementation shape must be explicit and justified.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "vm_sys_time.h"
#include "vm_device_support.h"

#ifndef MMBASIC_HOST
extern int64_t TimeOffsetToUptime;
#endif

static void vm_sys_time_set_mstring(uint8_t *out, const char *text) {
    size_t len = strlen(text);
    if (len > MAXSTRLEN) len = MAXSTRLEN;
    out[0] = (uint8_t)len;
    if (len) memcpy(out + 1, text, len);
}

#ifdef MMBASIC_HOST

void vm_sys_time_date(uint8_t *out) {
    vm_sys_time_set_mstring(out, "02-01-2024");
}

void vm_sys_time_time(uint8_t *out) {
    vm_sys_time_set_mstring(out, "03:04:05");
}

#else

static int vm_sys_time_now(struct tm *tm_out, uint64_t *now_us_out) {
    uint64_t now_us = readusclock();
    time_t epoch = (time_t)(now_us / 1000000ULL + TimeOffsetToUptime);
    struct tm *tm = gmtime(&epoch);
    if (!tm) return 0;
    *tm_out = *tm;
    if (now_us_out) *now_us_out = now_us;
    return 1;
}

void vm_sys_time_date(uint8_t *out) {
    struct tm tm;
    char text[32];

    if (!vm_sys_time_now(&tm, NULL)) {
        vm_sys_time_set_mstring(out, "00-00-0000");
        return;
    }

    snprintf(text, sizeof(text), "%02d-%02d-%04d",
             tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    vm_sys_time_set_mstring(out, text);
}

void vm_sys_time_time(uint8_t *out) {
    struct tm tm;
    char text[16];

    if (!vm_sys_time_now(&tm, NULL)) {
        vm_sys_time_set_mstring(out, "00:00:00");
        return;
    }

    snprintf(text, sizeof(text), "%02d:%02d:%02d",
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    vm_sys_time_set_mstring(out, text);
}

#endif
