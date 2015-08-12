/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Mihail Ivanchev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * A minimal, thread-safe exception throwing mechanism for C based on pthread.
 *
 * The general usage pattern is:
 *
 *   Exception *e;
 *
 *   exInit();
 *
 *   try
 *   {
 *   	... program statements ...
 *
 *   	exThrow(exOther, NULL, "Something happened.");
 *   }
 *   catch (e)
 *   {
 *   	switch(e->code)
 *   	{
 *   	default: ... process exception ...
 *   	}
 *
 *   	then either: exFree(e);
 *   	or: exRepeat(e);
 *   	or: exThrow(exOther, e, "Something else happened.");
 *   }
 *
 *   exDeinit();
 *
 * Consider the following points:
 *
 * - An exception consists of a code, a message and if necessary a cause.
 *
 * - A single catch block is expected.
 *
 * - There is no finally block and there will probably never be.
 *
 * - Do NOT return from within a try block, otherwise resources will be leaked.
 *
 * - You don't have to free, repeat or throw another exception in the catch
 *   block.
 *
 * - An exception has to be freed when it's no longer needed.
 *
 * - The exception pointer will be altered only when an exception is thrown.
 *   Keep this in mind if you set multiple exception traps in the body of a
 *   single function.
 *
 * - An exception can be the cause of at most 1 other exception.
 *
 * - While the implementation is thread-safe in the sense that the API is
 *   reentrant so you can safely throw and catch exceptions on different
 *   threads, you can still wreck quite a bit of havoc by doing illogical
 *   things or disregarding the API rules. Some examples of non-sense are:
 *   throwing an already freed exception; freeing an exception currently
 *   being thrown on another thread; throwing an exception which is
 *   currently being thrown on another thread.
 *
 * - No memory is dynamically allocated.
 */

#ifndef __EXCEPT_H__
#define __EXCEPT_H__

#include <setjmp.h>

#define MAX_MSG_LEN 2048

typedef enum
{
	/* Extend with additional codes. */
	exOther
} ExceptionCode;

typedef struct _Exception
{
	ExceptionCode code;
	char msg[MAX_MSG_LEN];
	struct _Exception * const cause;
} Exception;

#define try								\
	if (!setjmp(*pushCallingEnv())) {

#define catch(e)						\
		popCallingEnv(NULL);			\
	}									\
	else if (popCallingEnv(&e), 1)

/** Not part of the API, do not use. */
jmp_buf *pushCallingEnv();

/** Not part of the API, do not use. */
void popCallingEnv(Exception **e);

void exInit();
void exDeinit();
Exception* exAlloc(ExceptionCode code, Exception *cause, const char *msg, ...);
void exThrow(ExceptionCode code, Exception *cause, const char *msg, ...);
void exRepeat(Exception *e);
void exFree(Exception *e);

#endif // __EXCEPT_H__
