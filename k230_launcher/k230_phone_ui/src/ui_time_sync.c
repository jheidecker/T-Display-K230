#include "ui_time_sync.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define TIME_SYNC_LOG "/tmp/k230_clock_sync.log"
/* In /tmp, so it lasts one boot and is shared with the Meshtastic daemon */
#define TIME_SYNC_STATE "/tmp/k230_clock_sync.state"
#define TIME_SYNC_STATE_TMP TIME_SYNC_STATE ".tmp"
/* Earlier means the source doesn't know the time yet */
#define TIME_SYNC_MIN_EPOCH ((time_t)1767225600) /* 2026-01-01 */
/* Rejects two-digit-year defaults such as 80 read as 2080 */
#define TIME_SYNC_MAX_EPOCH ((time_t)2840140800LL) /* 2060-01-01 */
/* A source as good as the one that set the clock corrects it past this */
#define TIME_SYNC_CORRECT_US (5LL * 1000000LL)
/* A better source corrects it past this */
#define TIME_SYNC_UPGRADE_US (1LL * 1000000LL)

static pthread_mutex_t time_sync_lock = PTHREAD_MUTEX_INITIALIZER;
/* Priority of the source that set or confirmed the clock; 0 until one has */
static int time_sync_priority;

static const char *time_sync_source_name(k230_time_source_t source)
{
    switch(source) {
    case K230_TIME_SOURCE_LTE:
        return "LTE";
    case K230_TIME_SOURCE_GNSS:
        return "GNSS";
    case K230_TIME_SOURCE_NTP:
        return "NTP";
    }
    return "unknown";
}

/* LTE network time has 1 s resolution and arrives through a status poll */
static int time_sync_source_priority(k230_time_source_t source)
{
    return source == K230_TIME_SOURCE_LTE ? 1 : 2;
}

static uint64_t time_sync_monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int64_t time_sync_realtime_us(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static void time_sync_log(const char *fmt, ...)
{
    FILE *fp = fopen(TIME_SYNC_LOG, "a");
    va_list ap;

    if(!fp) {
        return;
    }
    fprintf(fp, "%llu ", (unsigned long long)time_sync_monotonic_us());
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

/* Another process may have set the clock since we last looked */
static void time_sync_load_locked(void)
{
    FILE *fp = fopen(TIME_SYNC_STATE, "r");
    int priority = 0;

    if(!fp) {
        return;
    }
    if(fscanf(fp, "priority=%d", &priority) == 1 &&
       priority > time_sync_priority) {
        time_sync_priority = priority;
    }
    fclose(fp);
}

static void time_sync_save_locked(k230_time_source_t source)
{
    FILE *fp = fopen(TIME_SYNC_STATE_TMP, "w");

    if(!fp) {
        return;
    }
    fprintf(fp, "priority=%d\nsource=%s\nepoch=%lld\n", time_sync_priority,
            time_sync_source_name(source), (long long)time(NULL));
    if(fclose(fp) == 0) {
        rename(TIME_SYNC_STATE_TMP, TIME_SYNC_STATE);
    }
}

time_t k230_time_sync_utc(int year, int month, int day, int hour, int min,
                          int sec)
{
    int64_t era;
    int y;
    int yoe;
    int doy;
    int64_t days;

    if(month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 ||
       hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
        return (time_t)-1;
    }
    /* Days from 1970-01-01 in the proleptic Gregorian calendar */
    y = year - (month <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (int)(y - era * 400);
    doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return (time_t)(days * 86400 + hour * 3600 + min * 60 + sec);
}

void k230_time_sync_offer(k230_time_source_t source, time_t utc,
                          uint64_t observed_us)
{
    const char *name = time_sync_source_name(source);
    int priority = time_sync_source_priority(source);
    uint64_t now_us = time_sync_monotonic_us();
    int64_t target_us;
    int64_t delta_us;
    int64_t limit_us;
    int first;
    struct timeval tv;

    if(utc < TIME_SYNC_MIN_EPOCH || utc >= TIME_SYNC_MAX_EPOCH) {
        return;
    }
    /* Carry the reading forward to now */
    target_us = (int64_t)utc * 1000000LL;
    if(observed_us != 0 && observed_us <= now_us) {
        target_us += (int64_t)(now_us - observed_us);
    }

    pthread_mutex_lock(&time_sync_lock);
    delta_us = target_us - time_sync_realtime_us();
    if(time_sync_priority == 0 || llabs(delta_us) >= TIME_SYNC_UPGRADE_US) {
        time_sync_load_locked();
    }
    first = time_sync_priority == 0;
    if(first) {
        limit_us = TIME_SYNC_UPGRADE_US;
    } else if(priority > time_sync_priority) {
        limit_us = TIME_SYNC_UPGRADE_US;
    } else if(priority == time_sync_priority) {
        limit_us = TIME_SYNC_CORRECT_US;
    } else {
        /* A worse source never overrides a better one */
        pthread_mutex_unlock(&time_sync_lock);
        return;
    }
    if(llabs(delta_us) < limit_us) {
        if(priority > time_sync_priority) {
            /* Already right: ntpd, or an earlier launcher run, set it */
            time_sync_priority = priority;
            time_sync_save_locked(source);
            pthread_mutex_unlock(&time_sync_lock);
            time_sync_log("clock confirmed by %s (%+.3f s)", name,
                          (double)delta_us / 1e6);
            return;
        }
        pthread_mutex_unlock(&time_sync_lock);
        return;
    }
    tv.tv_sec = (time_t)(target_us / 1000000LL);
    tv.tv_usec = (suseconds_t)(target_us % 1000000LL);
    if(settimeofday(&tv, NULL) != 0) {
        int err = errno;

        pthread_mutex_unlock(&time_sync_lock);
        time_sync_log("clock set from %s failed: %s", name, strerror(err));
        return;
    }
    if(priority > time_sync_priority) {
        time_sync_priority = priority;
    }
    time_sync_save_locked(source);
    pthread_mutex_unlock(&time_sync_lock);
    time_sync_log("clock %s from %s (%+.3f s)", first ? "set" : "corrected",
                  name, (double)delta_us / 1e6);
}

void k230_time_sync_note_set(k230_time_source_t source)
{
    int priority = time_sync_source_priority(source);

    pthread_mutex_lock(&time_sync_lock);
    time_sync_load_locked();
    if(priority > time_sync_priority) {
        time_sync_priority = priority;
    }
    time_sync_save_locked(source);
    pthread_mutex_unlock(&time_sync_lock);
    time_sync_log("clock set by %s", time_sync_source_name(source));
}
