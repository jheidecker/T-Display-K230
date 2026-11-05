#ifndef K230_PHONE_UI_TIME_SYNC_H
#define K230_PHONE_UI_TIME_SYNC_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The K230 has no RTC, so the clock starts at 1970 on every boot. The first
 * plausible time from any of these sources sets it; later readings only
 * correct it. NTP needs Wi-Fi, LTE network time needs registration, and GNSS
 * needs a fix, so whichever comes first wins.
 */
typedef enum {
    K230_TIME_SOURCE_LTE,  /* nRF9151 network time, AT+CCLK? */
    K230_TIME_SOURCE_GNSS, /* nRF9151 GNSS fix */
    K230_TIME_SOURCE_NTP,  /* ntpdate over Wi-Fi */
} k230_time_source_t;

/* UTC fields to seconds since the epoch; returns -1 for an invalid date */
time_t k230_time_sync_utc(int year, int month, int day, int hour, int min,
                          int sec);

/*
 * Offer a UTC reading. observed_us is when it was read, on CLOCK_MONOTONIC;
 * pass 0 for now. Readings before 2026 or after 2059 are ignored: they come
 * from a source that doesn't know the time yet.
 */
void k230_time_sync_offer(k230_time_source_t source, time_t utc,
                          uint64_t observed_us);

/* Record that something else, such as ntpdate, already set the clock */
void k230_time_sync_note_set(k230_time_source_t source);

#ifdef __cplusplus
}
#endif

#endif
