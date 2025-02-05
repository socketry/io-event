// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>
#include <time.h>

void IO_Event_Time_elapsed(struct timespec* start, struct timespec* stop, struct timespec *duration);
float IO_Event_Time_duration(struct timespec *duration);
void IO_Event_Time_current(struct timespec *time);

float IO_Event_Time_proportion(struct timespec *duration, struct timespec *total_duration);

#define IO_EVENT_TIME_PRINTF_TIMESPEC "%.3g"
#define IO_EVENT_TIME_PRINTF_TIMESPEC_ARGUMENTS(ts) ((double)(ts).tv_sec + (ts).tv_nsec / 1e9)
