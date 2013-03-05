/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Synchronization test code.
 */

#include <types.h>
#include <lib.h>
#include <clock.h>
#include <thread.h>
#include <synch.h>
#include <test.h>


#define NSEMLOOPS     63
#define NLOCKLOOPS    120
#define NCVLOOPS      5
#define NTHREADS      32

static volatile unsigned long testval1;
static volatile unsigned long testval2;
static volatile unsigned long testval3;
static struct semaphore *testsem;
static struct lock *testlock;
static struct cv *testcv;
static struct semaphore *donesem;

// Global
struct semaphore *driver;
struct semaphore *channel_1;
struct lock *cv_lock;

static
void
inititems(void)
{
	if (testsem==NULL) {
		testsem = sem_create("testsem", 2);
		if (testsem == NULL) {
			panic("synchtest: sem_create failed\n");
		}
	}
	if (testlock==NULL) {
		testlock = lock_create("testlock");
		if (testlock == NULL) {
			panic("synchtest: lock_create failed\n");
		}
	}
	if (testcv==NULL) {
		testcv = cv_create("testlock");
		if (testcv == NULL) {
			panic("synchtest: cv_create failed\n");
		}
	}
	if (donesem==NULL) {
		donesem = sem_create("donesem", 0);
		if (donesem == NULL) {
			panic("synchtest: sem_create failed\n");
		}
	}
}

static
void
semtestthread(void *junk, unsigned long num)
{
	int i;
	(void)junk;

	/*
	 * Only one of these should print at a time.
	 */
	P(testsem);
	kprintf("Thread %2lu: ", num);
	for (i=0; i<NSEMLOOPS; i++) {
		kprintf("%c", (int)num+64);
	}
	kprintf("\n");
	V(donesem);
}

int
semtest(int nargs, char **args)
{
	int i, result;

	(void)nargs;
	(void)args;

	inititems();
	kprintf("Starting semaphore test...\n");
	kprintf("If this hangs, it's broken: ");
	P(testsem);
	P(testsem);
	kprintf("ok\n");

	for (i=0; i<NTHREADS; i++) {
		result = thread_fork("semtest", semtestthread, NULL, i, NULL);
		if (result) {
			panic("semtest: thread_fork failed: %s\n",
			      strerror(result));
		}
	}

	for (i=0; i<NTHREADS; i++) {
		V(testsem);
		P(donesem);
	}

	/* so we can run it again */
	V(testsem);
	V(testsem);

	kprintf("Semaphore test done.\n");
	return 0;
}

static
void
fail(unsigned long num, const char *msg)
{
	kprintf("thread %lu: Mismatch on %s\n", num, msg);
	kprintf("Test failed\n");

	lock_release(testlock);

	V(donesem);
	thread_exit();
}

static
void
locktestthread(void *junk, unsigned long num)
{
	int i;
	(void)junk;

	for (i=0; i<NLOCKLOOPS; i++) {
		lock_acquire(testlock);
		testval1 = num;
		testval2 = num*num;
		testval3 = num%3;

		if (testval2 != testval1*testval1) {
			fail(num, "testval2/testval1");
		}

		if (testval2%3 != (testval3*testval3)%3) {
			fail(num, "testval2/testval3");
		}

		if (testval3 != testval1%3) {
			fail(num, "testval3/testval1");
		}

		if (testval1 != num) {
			fail(num, "testval1/num");
		}

		if (testval2 != num*num) {
			fail(num, "testval2/num");
		}

		if (testval3 != num%3) {
			fail(num, "testval3/num");
		}

		lock_release(testlock);
	}
	V(donesem);
}


int
locktest(int nargs, char **args)
{
	int i, result;

	(void)nargs;
	(void)args;

	inititems();
	kprintf("Starting lock test...\n");

	for (i=0; i<NTHREADS; i++) {
		result = thread_fork("synchtest", locktestthread, NULL, i,
				     NULL);
		if (result) {
			panic("locktest: thread_fork failed: %s\n",
			      strerror(result));
		}
	}
	for (i=0; i<NTHREADS; i++) {
		P(donesem);
	}

	kprintf("Lock test done.\n");

	return 0;
}

// STARTS UNIT TESTS FOR LOCK

static void
test_lock_create(){
	struct lock *lk = lock_create("lk");

	KASSERT(!strcmp(lk->lk_name, "lk"));
	//KASSERT(lk->l_wchan != NULL);
	KASSERT(lk->holder == NULL);

	lock_destroy(lk);

	kprintf("test_lock_create: Passed.....\n");
}

static void
test_holder_helper(void *p, unsigned long i){
	(void)i;
	struct lock *lk = p;
	lock_release(lk); // THE KERNEL SHOULD PANIC!
}

static void
test_holder(){
	kprintf("test_holder: this test should fail with the following message when run:\n\
		panic: Assertion failed: lock->holder == curthread, at ../../thread/synch.c:217 (lock_release)\n");

	struct lock *lk = lock_create("lk");
	lock_acquire(lk);

	int err = thread_fork("test_holder_helper", test_holder_helper, (char *)lk, 0, NULL);
	if (err) {
		panic("test_holder: thread_fork failed: %s\n", strerror(err));
	}
}

static void
test_do_i_hold_helper(void *p, unsigned long i){
	(void)i;
	struct lock *lk = p;
	KASSERT(!lock_do_i_hold(lk));

	V(channel_1);
}

static void
test_do_i_hold(){
	channel_1 = sem_create("channel 1", 0);
	struct lock *lk = lock_create("lk");
	lock_acquire(lk);
	KASSERT(lock_do_i_hold(lk));

	int err = thread_fork("test_do_i_hold_helper", test_do_i_hold_helper, \
		(char *)lk, 0, NULL);
	if (err) {
		panic("test_do_i_hold: thread_fork failed: %s\n", strerror(err));
	}

	// Clean up
	P(channel_1);
	lock_release(lk);
	lock_destroy(lk);
	sem_destroy(channel_1);

	kprintf("test_do_i_hold: Passed.....\n");
}

static void
test_lock_destroy(){
	struct lock *lk = lock_create("lk");
	lock_destroy(lk);

	lock_acquire(lk); // This should lead the spinlock to spin forever
}

static void
helper(void *p, unsigned long i){
	(void)i;
	struct lock *lk = p;
	lock_acquire(lk);
	kprintf("Thread %d acquired the lock\n", (int)i);
	lock_release(lk);

	V(channel_1);
}

static void
test_acquire_release(){
	int i;

	channel_1 = sem_create("channel 1", 0);
	struct lock *lk = lock_create("lk");

	// Fork 10 threads that all try to acquire, then release the lock
	for(i=0;i<10;i++){
		int err = thread_fork("helper", helper, \
			(char *)lk, i, NULL);
		if (err) {
			panic("test_acquire_release: thread_fork failed: %s\n", strerror(err));
		}
	}

	// Clean up
	for(i=0; i<10; i++){
		P(channel_1);
	}
	lock_destroy(lk);
	sem_destroy(channel_1);

	kprintf("test_acquire_release: Passed.....\n");
	
	V(driver); // Placed at the end of the last unit test
}

int lock_unittest(int nargs, char **args){
	(void)nargs;
	(void)args;

	driver = sem_create("driver", 0);

	kprintf("Starting Unit Test Suite for Locks..........\n");

	/* Test that lock_create() creates a lock and lk_name, l_chan, and holder
	   are properly initialized. */
	test_lock_create();

	/* Test that only the thread holding the lock may release it; commented out
	   intentionally. */
	if(0)
		test_holder();
	kprintf("test_holder: Passed.....\n");

	/* Test that lock_do_i_hold() returns whether the current thread holds the
	   lock for both cases. */
	test_do_i_hold();

	/* Test lock_destroy() loops forever as the spinlock_acquire() tries to
	   acquire a spinlock that has been destroyed but not set to NULL.
	   Commented out intentionally. */
	if(0)
		test_lock_destroy();
	kprintf("test_lock_destroy: Passed.....\n");

	/* Test lock_acquire() and lock_release by ensuring that all 10 thread
	   seeking a lock will eventually get it as long as the lock is eventually
	   released by the holder. */
	test_acquire_release();

	// Synchronize with menu
	P(driver);
	sem_destroy(driver);

	return 0;
}

// ENDS UNIT TESTS FOR LOCK


// STARTS UNIT TESTS FOR CV

static void
test_cv_create(){
	struct cv *cv = cv_create("cv");

	KASSERT(!strcmp(cv->cv_name, "cv"));
	KASSERT(cv->cv_wchan != NULL);

	cv_destroy(cv);

	kprintf("test_cv_create: Passed.....\n");
}


static void
test_cv_signal_helper(void *p, unsigned long i){
	(void)i;
	struct cv *cv = p;

	lock_acquire(cv_lock);
	V(channel_1);
	cv_wait(cv, cv_lock);
	kprintf("Signaled!\n");
	lock_release(cv_lock);

	V(channel_1);
}

static void
test_cv_signal(){
	int i;
	struct cv *cv = cv_create("cv");
	cv_lock = lock_create("cv lock");
	channel_1 = sem_create("channel 1", 0);

	kprintf("Signal recieved should only print once:\n");

	// Fork 2 threads, only 1 of which should recieve the signal
	for(i=0;i<2;i++){
		int err = thread_fork("test_cv_signal_helper", test_cv_signal_helper, \
			(char *)cv, 0, NULL);
		if (err) {
			panic("test_cv_signal_helper: thread_fork failed: %s\n", strerror(err));
		}
	}
	// Wait for threads to be ready
	for(i=0;i<2;i++){
		P(channel_1);
	}

	lock_acquire(cv_lock);
	cv_signal(cv, cv_lock);
	lock_release(cv_lock);

	// Clean up
	P(channel_1);
	sem_destroy(channel_1);

	kprintf("test_cv_signal: Passed.....\n");
}

static void
test_cv_broadcast_helper(void *p, unsigned long i){
	struct cv *cv = p;

	lock_acquire(cv_lock);
	V(channel_1);
	cv_wait(cv, cv_lock);
	kprintf("Thread %d signaled!\n", (int) i);
	lock_release(cv_lock);

	V(channel_1);
}

static void
test_cv_broadcast(){
	int i;
	struct cv *cv = cv_create("cv");
	cv_lock = lock_create("cv lock");
	channel_1 = sem_create("channel 1", 0);

	// Fork 10 threads; all should recieve the signal
	for(i=0;i<10;i++){
		int err = thread_fork("test_cv_broadcast_helper", test_cv_broadcast_helper, \
			(char *)cv, i, NULL);
		if (err) {
			panic("test_cv_broadcast_helper: thread_fork failed: %s\n", strerror(err));
		}
	}
	// Wait for threads to be ready
	for(i=0;i<10;i++){
		P(channel_1);
	}

	lock_acquire(cv_lock);
	cv_broadcast(cv, cv_lock);
	lock_release(cv_lock);

	// Clean up
	for(i=0;i<10;i++){
		P(channel_1);
	}

	cv_destroy(cv);
	lock_destroy(cv_lock);
	sem_destroy(channel_1);

	kprintf("test_cv_broadcast: Passed.....\n");

	V(driver); // Placed at the end of the last unit test
}

int cv_unittest(int nargs, char **args){
	(void)nargs;
	(void)args;

	driver = sem_create("driver", 0);

	kprintf("Starting Unit Test Suite for CVs..........\n");

	/* Test that cv_create() creates a cv with cv_name and cv_wchan properly
	   initialized. */
	test_cv_create();

	/* Test that cv_signal() signals 1 waiting thread and only 1. */
	test_cv_signal();

	/* Test that cv_broadcast signals all waiting threads. */
	test_cv_broadcast();

	// Synchronize with menu
	P(driver);
	sem_destroy(driver);

	return 0;
}


// ENDS UNIT TESTS FOR CV

static
void
cvtestthread(void *junk, unsigned long num)
{
	int i;
	volatile int j;
	time_t secs1, secs2;
	uint32_t nsecs1, nsecs2;

	(void)junk;

	for (i=0; i<NCVLOOPS; i++) {
		lock_acquire(testlock);
		while (testval1 != num) {
			gettime(&secs1, &nsecs1);
			cv_wait(testcv, testlock);
			gettime(&secs2, &nsecs2);

			if (nsecs2 < nsecs1) {
				secs2--;
				nsecs2 += 1000000000;
			}

			nsecs2 -= nsecs1;
			secs2 -= secs1;

			/* Require at least 2000 cpu cycles (we're 25mhz) */
			if (secs2==0 && nsecs2 < 40*2000) {
				kprintf("cv_wait took only %u ns\n", nsecs2);
				kprintf("That's too fast... you must be "
					"busy-looping\n");
				V(donesem);
				thread_exit();
			}

		}
		kprintf("Thread %lu\n", num);
		testval1 = (testval1 + NTHREADS - 1)%NTHREADS;

		/*
		 * loop a little while to make sure we can measure the
		 * time waiting on the cv.
		 */
		for (j=0; j<3000; j++);

		cv_broadcast(testcv, testlock);
		lock_release(testlock);
	}
	V(donesem);
}

int
cvtest(int nargs, char **args)
{

	int i, result;

	(void)nargs;
	(void)args;

	inititems();
	kprintf("Starting CV test...\n");
	kprintf("Threads should print out in reverse order.\n");

	testval1 = NTHREADS-1;

	for (i=0; i<NTHREADS; i++) {
		result = thread_fork("synchtest", cvtestthread, NULL, i,
				      NULL);
		if (result) {
			panic("cvtest: thread_fork failed: %s\n",
			      strerror(result));
		}
	}
	for (i=0; i<NTHREADS; i++) {
		P(donesem);
	}

	kprintf("CV test done\n");

	return 0;
}
