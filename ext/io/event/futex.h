// Released under the MIT License.
// Copyright, 2026, by Samuel Williams.

#pragma once

#include <ruby.h>

#ifdef HAVE_RUBY_IO_BUFFER_H
#include <ruby/io/buffer.h>
#endif

// Version 3 (Ruby 4.1) provides counted allocation locks, allowing independent
// futex words to retain the same buffer without unlocking each other.
#if defined(__linux__) && RUBY_IO_BUFFER_VERSION >= 3 && defined(HAVE_LINUX_FUTEX_H) && defined(HAVE_SYS_SYSCALL_H)

#define IO_EVENT_FUTEX

#include <linux/futex.h>
#include <stdint.h>
#include <sys/syscall.h>

#ifndef FUTEX2_SIZE_U32
#define FUTEX2_SIZE_U32 2
#endif

#ifndef FUTEX_32
#define FUTEX_32 2
#endif

#if defined(FUTEX_WAITV_MAX) && defined(SYS_futex_waitv)
#define IO_EVENT_FUTEX_WAITV
#endif

uint32_t *IO_Event_Futex_address(VALUE self);
uint32_t *IO_Event_Futex_acquire(VALUE self);
void IO_Event_Futex_release(VALUE self);
#ifdef IO_EVENT_FUTEX_WAITV
VALUE IO_Event_Futex_prepare_waitv(VALUE entries, struct futex_waitv *vector);
#endif
void Init_IO_Event_Futex(VALUE IO_Event);

#endif
