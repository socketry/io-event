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

#include <ruby.h>
#include <ruby/thread.h>

enum Event {
	READABLE = 1,
	PRIORITY = 2,
	WRITABLE = 4,
	ERROR = 8,
	HANGUP = 16
};

void
Init_Event_Backend();

VALUE Event_Backend_transfer(VALUE fiber);
VALUE Event_Backend_transfer_result(VALUE fiber, VALUE argument);

VALUE Event_Backend_process_status_wait(rb_pid_t pid);

char* Event_Backend_verify_size(VALUE buffer, size_t offset, size_t length);
char* Event_Backend_resize_to_capacity(VALUE string, size_t offset, size_t length);
void Event_Backend_resize_to_fit(VALUE string, size_t offset, size_t length);

int Event_Backend_nonblock_set(int file_descriptor);
void Event_Backend_nonblock_restore(int file_descriptor, int flags);
