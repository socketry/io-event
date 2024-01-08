// Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once

#include <ruby.h>

#ifdef HAVE_SYS_EVENTFD_H
struct IO_Event_Interrupt {
	int descriptor;
};

static inline int IO_Event_Interrupt_descriptor(struct IO_Event_Interrupt *interrupt) {
	return interrupt->descriptor;
}
#else
struct IO_Event_Interrupt {
	int descriptor[2];
};

static inline int IO_Event_Interrupt_descriptor(struct IO_Event_Interrupt *interrupt) {
	return interrupt->descriptor[0];
}
#endif

void IO_Event_Interrupt_open(struct IO_Event_Interrupt *interrupt);
void IO_Event_Interrupt_close(struct IO_Event_Interrupt *interrupt);

void IO_Event_Interrupt_signal(struct IO_Event_Interrupt *interrupt);
void IO_Event_Interrupt_clear(struct IO_Event_Interrupt *interrupt);
