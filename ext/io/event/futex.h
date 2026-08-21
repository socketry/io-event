// Released under the MIT License.
// Copyright, 2026, by Samuel Williams.

#pragma once

#include <ruby.h>

#if defined(__linux__) && defined(HAVE_RUBY_IO_BUFFER_H) && defined(HAVE_LINUX_FUTEX_H) && defined(HAVE_SYS_SYSCALL_H)

#define IO_EVENT_FUTEX

#include <linux/futex.h>
#include <stdint.h>

#ifndef FUTEX2_SIZE_U32
#define FUTEX2_SIZE_U32 2
#endif

#ifndef FUTEX_32
#define FUTEX_32 2
#endif

#ifndef FUTEX_WAITV_MAX
#define FUTEX_WAITV_MAX 128
#endif

uint32_t *IO_Event_Futex_address(VALUE self);
void Init_IO_Event_Futex(VALUE IO_Event);

#endif
