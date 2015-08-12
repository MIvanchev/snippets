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

#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "except.h"

#define ENV_LIST_SIZE 16
#define EXCEPTION_LIST_SIZE 16

typedef struct _EnvEntry
{
	jmp_buf env;
	int used;
	int level;
	pthread_t thread;
	struct _EnvEntry *next;
} EnvEntry;

typedef struct _ExceptionEntry
{
	Exception exception;
	struct
	{
		int used :1;
		int thrown :1;
		int cause :1;
	};
	pthread_t thread;
	struct _ExceptionEntry *next;
} ExceptionEntry;

static EnvEntry envList[ENV_LIST_SIZE];
static ExceptionEntry exceptionList[EXCEPTION_LIST_SIZE];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

EnvEntry* getLastEnvEntry(pthread_t thread)
{
	EnvEntry *entry, *last = NULL;
	int i;

	for (i = ENV_LIST_SIZE - 1; i >= 0; i--)
	{
		entry = &envList[i];

		if (entry->used && entry->thread == thread)
		{
			if (!last || entry->level > last->level)
				last = entry;
		}
	}

	return last;
}

ExceptionEntry* getExceptionEntry(Exception *e)
{
	ExceptionEntry *entry;

	for (entry = exceptionList; entry; entry = entry->next)
	{
		if (&entry->exception == e)
			break;
	}

	return entry;
}

ExceptionEntry* allocExceptionEntry()
{
	ExceptionEntry *entry;

	for (entry = exceptionList; entry; entry = entry->next)
	{
		if (!entry->used)
		{
			entry->cause = 0;
			entry->used = 1;
			break;
		}
	}

	return entry;
}

jmp_buf* pushCallingEnv()
{
	pthread_t thread = pthread_self();
	EnvEntry *entry, *unused = NULL;
	int level = 0;

	pthread_mutex_lock(&mutex);

	for (entry = envList; entry; entry = entry->next)
	{
		if (!unused && !entry->used)
			unused = entry;
		else if (entry->used && entry->thread == thread)
			level++;
	}

	unused->used = 1;
	unused->level = level;
	unused->thread = thread;

	pthread_mutex_unlock(&mutex);
	return &unused->env;
}

void popCallingEnv(Exception **e)
{
	pthread_t thread = pthread_self();
	ExceptionEntry *entry;

	pthread_mutex_lock(&mutex);
	getLastEnvEntry(thread)->used = 0;

	for (entry = exceptionList; entry; entry = entry->next)
	{
		if (entry->used && entry->thrown && entry->thread == thread)
		{
			entry->thrown = 0;
			pthread_mutex_unlock(&mutex);

			*e = &entry->exception;
			return;
		}
	}

	pthread_mutex_unlock(&mutex);
}

void exInit()
{
	int i;

	envList[ENV_LIST_SIZE - 1].next = NULL;
	for (i = 1; i < ENV_LIST_SIZE; i++)
		envList[i - 1].next = &envList[i];

	exceptionList[ENV_LIST_SIZE - 1].next = NULL;
	for (i = 1; i < EXCEPTION_LIST_SIZE; i++)
		exceptionList[i - 1].next = &exceptionList[i];
}

void exDeinit()
{
#ifndef NDEBUG
	int i;

	for (i = 0; i < ENV_LIST_SIZE; i++)
	{
		if (envList[i].used)
			printf("A calling environment was not freed. Did you exit a function from a try block?");
	}

	for (i = 0; i < EXCEPTION_LIST_SIZE; i++)
	{
		if (exceptionList[i].used)
		{
			exceptionList[i].exception.msg[MAX_MSG_LEN - 1] = '\0';
			printf("An exception was not freed. The message is: %s.", exceptionList[i].exception.msg);
		}
	}
#endif
}

Exception* exAlloc(ExceptionCode code, Exception *cause, const char *msg, ...)
{
	ExceptionEntry *entry;
	Exception *e;
	va_list argList;

	pthread_mutex_lock(&mutex);

	assert(!cause || getExceptionEntry(cause)->used);
	assert(!cause || !getExceptionEntry(cause)->cause);
	assert(!cause || !getExceptionEntry(cause)->thrown);

	entry = allocExceptionEntry();
	if (cause)
		getExceptionEntry(cause)->cause = 1;

	pthread_mutex_unlock(&mutex);

	e = &entry->exception;
	e->code = code;
	*((Exception **) &e->cause) = cause;

	va_start(argList, msg);
	vsnprintf(e->msg, MAX_MSG_LEN, msg, argList);
	va_end(argList);

	return e;
}

void exThrow(ExceptionCode code, Exception *cause, const char *msg, ...)
{
	ExceptionEntry *entry;
	Exception *e;
	jmp_buf *env;
	pthread_t thread = pthread_self();
	va_list argList;

	pthread_mutex_lock(&mutex);

	assert(!cause || getExceptionEntry(cause)->used);
	assert(!cause || !getExceptionEntry(cause)->cause);
	assert(!cause || !getExceptionEntry(cause)->thrown);

	entry = allocExceptionEntry();
	entry->thrown = 1;
	entry->thread = thread;
	env = &getLastEnvEntry(thread)->env;

	if (cause)
		getExceptionEntry(cause)->cause = 1;

	pthread_mutex_unlock(&mutex);

	e = &entry->exception;
	e->code = code;
	*((Exception **) &e->cause) = cause;

	va_start(argList, msg);
	vsnprintf(e->msg, MAX_MSG_LEN, msg, argList);
	va_end(argList);

	longjmp(*env, 1);
}

void exRepeat(Exception *e)
{
	ExceptionEntry *entry = getExceptionEntry(e);
	pthread_t thread = pthread_self();
	jmp_buf *env;

	pthread_mutex_lock(&mutex);

	assert(entry->used);
	assert(!entry->thrown);

	entry->thrown = 1;
	entry->thread = thread;
	env = &getLastEnvEntry(thread)->env;

	pthread_mutex_unlock(&mutex);

	longjmp(*env, 1);
}

void exFree(Exception *e)
{
	pthread_mutex_lock(&mutex);

	assert(getExceptionEntry(e)->used);
	assert(!getExceptionEntry(e)->cause);
	assert(!getExceptionEntry(e)->thrown);

	for (; e; e = e->cause)
		getExceptionEntry(e)->used = 0;

	pthread_mutex_unlock(&mutex);
}

#ifdef TEST

#include <assert.h>
#include <time.h>

typedef struct
{
	int used;
	pthread_t id;
	int secrets[3];
} TestThread;

#define NUM_THREADS 5
#define NUM_SECONDS 10

static TestThread testThreads[NUM_THREADS];
static pthread_mutex_t testMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t testCond = PTHREAD_COND_INITIALIZER;
static clock_t initTime;
static clock_t lastTime;
static int totalThreads;

static void funcB(TestThread *thread)
{
	Exception *e;
	EnvEntry *entry;
	int i = 0;

	try
	{
		try
			exThrow(exOther, NULL, "No error.");
		catch (e)
		{
			exFree(e);
			e = exAlloc(exOther, NULL, "%d", thread->secrets[0]);
			exThrow(exOther, e, "%d", thread->secrets[1]);
		}
	}
	catch (e)
	exRepeat(e);
}

static void* funcA(void *data)
{
	TestThread *thread = (TestThread*) data;
	Exception *e;
	char msgExpected[MAX_MSG_LEN];
	char msgActual[MAX_MSG_LEN];

	try
	{
		try
			funcB(thread);
		catch (e)
		{
			snprintf(e->msg + strlen(e->msg), MAX_MSG_LEN, " %d", thread->secrets[2]);
			exRepeat(e);
		}
	}
	catch (e)
	{
		snprintf(msgActual, MAX_MSG_LEN, "%s %s", e->cause->msg, e->msg);
		exFree(e);
	}

	pthread_mutex_lock(&testMutex);

	/* Comparison inside the mutex since assert might not be thread-safe. */

	snprintf(msgExpected, MAX_MSG_LEN, "%d %d %d", thread->secrets[0], thread->secrets[1], thread->secrets[2]);
	assert(!strcmp(msgExpected, msgActual));

	thread->used = 0;
	pthread_cond_signal(&testCond);
	pthread_mutex_unlock(&testMutex);

	return NULL;
}

int main(void)
{
	int i;

	printf("Testing try-catch for up %d seconds with %d threads simultaneously.\n", NUM_SECONDS, NUM_THREADS);

	exInit();

	clock_t initTime = clock();
	clock_t lastTime = initTime;

#define TIME(t) ((clock() - t) / CLOCKS_PER_SEC)

	while (TIME(initTime) < NUM_SECONDS)
	{
		pthread_mutex_lock(&testMutex);

		if (TIME(lastTime) > 1)
		{
			printf("After %.1f seconds: successfully tested with %d threads.\n", (double) TIME(initTime), totalThreads);
			lastTime = clock();
		}

		for (i = 0; i < NUM_THREADS; i++)
		{
			if (!testThreads[i].used)
			{
				pthread_join(testThreads[i].id, NULL);

				testThreads[i].secrets[0] = rand();
				testThreads[i].secrets[1] = rand();
				testThreads[i].secrets[2] = rand();
				testThreads[i].used = 1;
				totalThreads++;

				pthread_create(&testThreads[i].id, NULL, funcA, &testThreads[i]);
			}
		}

		pthread_cond_wait(&testCond, &testMutex);
		pthread_mutex_unlock(&testMutex);
	}

	for (i = 0; i < NUM_THREADS; i++)
		pthread_join(testThreads[i].id, NULL);

	for (i = 0; i < ENV_LIST_SIZE; i++)
		assert(!envList[i].used);

	for (i = 0; i < EXCEPTION_LIST_SIZE; i++)
		assert(!exceptionList[i].used);

	printf("Successfully tested with %d threads.\n\n", totalThreads);

	exDeinit();
	fflush(stdout);

	return 0;
}

#endif
