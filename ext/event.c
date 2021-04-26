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

#include "event.h"

VALUE Event = Qnil;
VALUE Event_Backend = Qnil;

void Init_event()
{
	#ifdef HAVE_RB_EXT_RACTOR_SAFE
	rb_ext_ractor_safe(true);
	#endif
	
	Event = rb_define_module("Event");
	Event_Backend = rb_define_module_under(Event, "Backend");
	
	#ifdef EVENT_BACKEND_URING
	Init_Event_Backend_IOUring(Event_Backend);
	#endif
	
	#ifdef EVENT_BACKEND_EPOLL
	Init_Event_Backend_EPoll(Event_Backend);
	#endif
	
	#ifdef EVENT_BACKEND_KQUEUE
	Init_Event_Backend_KQueue(Event_Backend);
	#endif
}
