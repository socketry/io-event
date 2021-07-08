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

#include "backend.h"
#include <fcntl.h>

static ID id_transfer, id_wait;
static VALUE rb_Process_Status = Qnil;

void Init_Event_Backend(VALUE Event_Backend) {
	id_transfer = rb_intern("transfer");
	id_wait = rb_intern("wait");
	// id_alive_p = rb_intern("alive?");
	rb_Process_Status = rb_const_get_at(rb_mProcess, rb_intern("Status"));
}

VALUE
Event_Backend_transfer(VALUE fiber) {
#ifdef HAVE__RB_FIBER_TRANSFER
	return rb_fiber_transfer(fiber, 0, NULL);
#else
	return rb_funcall(fiber, id_transfer, 0);
#endif
}

VALUE
Event_Backend_transfer_result(VALUE fiber, VALUE result) {
	// if (!RTEST(rb_fiber_alive_p(fiber))) {
	// 	return Qnil;
	// }
	
#ifdef HAVE__RB_FIBER_TRANSFER
	return rb_fiber_transfer(fiber, 1, &result);
#else
	return rb_funcall(fiber, id_transfer, 1, result);
#endif
}

VALUE Event_Backend_process_status_wait(rb_pid_t pid)
{
	return rb_funcall(rb_Process_Status, id_wait, 2, PIDT2NUM(pid), INT2NUM(WNOHANG));
}

char* Event_Backend_verify_size(VALUE buffer, size_t offset, size_t length) {
	if ((size_t)RSTRING_LEN(buffer) < offset + length) {
		rb_raise(rb_eRuntimeError, "invalid offset/length exceeds bounds of buffer");
	}
	
	return RSTRING_PTR(buffer);
}

int Event_Backend_nonblock_set(int file_descriptor)
{
	int flags = fcntl(file_descriptor, F_GETFL, 0);
	
	if (!(flags & O_NONBLOCK)) {
		fcntl(file_descriptor, F_SETFL, flags | O_NONBLOCK);
	}
	
	return flags;
}

void Event_Backend_nonblock_restore(int file_descriptor, int flags)
{
	if (!(flags & O_NONBLOCK)) {
		fcntl(file_descriptor, F_SETFL, flags & ~flags);
	}
}