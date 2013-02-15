/*
 * Copyright (c) 2013
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

/* Correctness criteria:
- There should be NFOTRS * (HOBBITS_PER_FOTR + MEN_PER_FOTR + 3) lines of output
  indicating that someone is leaving, and HOBBITS_PER_FOTR lines of output
  indivating that a fellowship is formed. Using the default values in common.h,
  this would be 90 outputs of someone leaving, and 10 outputs of fellowships
  being formed for a total of 100 outputs. Note that this is under the
  assumption that threads are pawned the way that they are provided in the
  driver code. If we modify the radio of races to NFOTRS, then we need to
  replace the NFOTRS value used above with:
    Min(TOTAL_HOBBITS/4, TOTAL_MEN/2, TOTAL_OF_EACH_OF_THE_OTHER_RACES)
  Note that I did not explicitly test for such a scenario since that would be
  modifying the given driver code.
- No one should leave twice (unless there are duplicate names, which I don't
  believe there to be).
- One has to leave AFTER his fellowship is formed, not before. However one can
  leave any time after, regardless of when other fellowships are formed.

  By running fotr and painstakingly parsing the data, all of the above
  correctness criteria have been met.
*/

/**
 * Driver code for The Fellowship of the Ring synch problem.
 */

#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>

#include "common.h"


///////////////////////////////////////////////////////////////////////////////
//
//  Name functions for the races of Middle-Earth.
//
//  Your solution should print NFOTRS full fellowships to stdout, each on a
//  separate line.  Each such fellowship should have the form:
//
//    FELLOWSHIP: wizard, man, man, elf, dwarf, hobbit, hobbit, hobbit, hobbit
//
//  where each member of each race is identified by name using these helper
//  routines (e.g., nameof_istari()). The threads can exit once the full
//  fellowship is printed, and should also print out their names as they do so:
//
//    LEAVING: name
//

#define NAMEOF_FUNC(race)   \
  static const char *       \
  nameof_##race(int which)  \
  {                         \
    return race[which];     \
  }

NAMEOF_FUNC(istari);
NAMEOF_FUNC(menfolk);
NAMEOF_FUNC(eldar);
NAMEOF_FUNC(khazad);
NAMEOF_FUNC(hobbitses);

#undef NAMEOF_FUNC

struct fellowship {
  char *names[9];
  int total;

  // Numbers of each
  int wizard;
  int man;
  int elf;
  int dwarf;
  int hobbit;

  struct lock *fellowship_lk;
  struct lock *cv_lk;
  struct cv *ready;
};

struct semaphore *driver_sem;

typedef enum
{
  WIZARD, MAN, ELF, DWARF, HOBBIT
} race;

// Global
struct fellowship *fs;
struct semaphore *print_lock;
struct lock *cleanup_lock;
int count;

///////////////////////////////////////////////////////////////////////////////
//
//  Driver code.
//
//  TODO: Implement all the thread entrypoints!
//

static void
join(char* name, race r){
  int i,j;

  // Try to join each fellowship
  for(i=0; i<NFOTRS; i++){
    lock_acquire(fs[i].fellowship_lk);

    // Check space availability
    int available = 0;
    switch(r)
    {
      case WIZARD:
        available = (fs[i].wizard < 1);
        break;
      case MAN:
        available = (fs[i].man < 2);
        break;
      case ELF:
        available = (fs[i].elf < 1);
        break;
      case DWARF:
        available = (fs[i].dwarf < 1);
        break;
      case HOBBIT:
        available = (fs[i].hobbit < 4);
        break;
    }

    if(available){

      // Claim a spot
      fs[i].names[fs[i].total] = kstrdup(name);
      fs[i].total++;
      switch(r)
      {
        case WIZARD:
          fs[i].wizard++;
          break;
        case ELF:
          fs[i].elf++;
          break;
        case DWARF:
          fs[i].dwarf++;
          break;
        case MAN:
          fs[i].man++;
          break;
        case HOBBIT:
          fs[i].hobbit++;
          break;
      }

      // Check if the fellowship is formed
      if(fs[i].total == 9){
        // Fellowship formed!
        P(print_lock);
        kprintf("FELLOWSHIP:\t%s", fs[i].names[0]);
        for(j=1; j<9; j++){
          kprintf(", %s", fs[i].names[j]);
        }
        kprintf("\n");
        kprintf("LEAVING:\t%s\n", name);
        V(print_lock);

        // Rally the fellowship
        lock_acquire(fs[i].cv_lk);
        cv_broadcast(fs[i].ready, fs[i].cv_lk);
        lock_release(fs[i].cv_lk);
        lock_release(fs[i].fellowship_lk);

        // For cleanup
        lock_acquire(cleanup_lock);
        count++;
        lock_release(cleanup_lock);
      }

      // Fellowship not ready, wait
      else{
        lock_acquire(fs[i].cv_lk);
        lock_release(fs[i].fellowship_lk);
        cv_wait(fs[i].ready, fs[i].cv_lk);

        // Leave for quest
        lock_release(fs[i].cv_lk);
        P(print_lock);
        kprintf("LEAVING:\t%s\n", name);
        V(print_lock);

        // For cleanup
        lock_acquire(cleanup_lock);
        count++;
        lock_release(cleanup_lock);
      }
    }

    // No space; check next followship
    else{
      lock_release(fs[i].fellowship_lk);
    }
  }
  // Cleanup memory
  if(count == 9 * NFOTRS)
    V(driver_sem);
}

static void
wizard(void *p, unsigned long which)
{
  (void)p;
  char *name = kstrdup(nameof_istari(which));
  join(name, WIZARD);
  kfree(name);
}

static void
man(void *p, unsigned long which)
{
  (void)p;
  char *name = kstrdup(nameof_menfolk(which));
  join(name, MAN);
  kfree(name);
}

static void
elf(void *p, unsigned long which)
{
  (void)p;
  char *name = kstrdup(nameof_eldar(which));
  join(name, ELF);
  kfree(name);
}

static void
dwarf(void *p, unsigned long which)
{
  (void)p;
  char *name = kstrdup(nameof_khazad(which));
  join(name, DWARF);
  kfree(name);
}

static void
hobbit(void *p, unsigned long which)
{
  (void)p;
  char *name = kstrdup(nameof_hobbitses(which));
  join(name, HOBBIT);
  kfree(name);
}

/**
 * fellowship - Fellowship synch problem driver routine.
 *
 * You may modify this function to initialize any synchronization primitives
 * you need; however, any other data structures you need to solve the problem
 * must be handled entirely by the forked threads (except for some freeing at
 * the end).  Feel free to change the thread forking loops if you wish to use
 * the same entrypoint routine to implement multiple Middle-Earth races.
 *
 * Make sure you don't leak any kernel memory!  Also, try to return the test to
 * its original state so it can be run again.
 */
int
fellowship(int nargs, char **args)
{
  int i, j, n;

  (void)nargs;
  (void)args;

  // Initialize fellowships
  fs = kmalloc(NFOTRS * sizeof(struct fellowship));
  print_lock = sem_create("print lock", 1);
  cleanup_lock = lock_create("cleanup lock");
  driver_sem = sem_create("driver semaphore", 0);
  count = 0;

  for (i=0; i<NFOTRS; i++){
    fs[i].fellowship_lk = lock_create("fellowship_lk");
    fs[i].cv_lk = lock_create("cv_lk");
    fs[i].ready = cv_create("ready");
  }

  for (i = 0; i < NFOTRS; ++i) {
    thread_fork_or_panic("wizard", wizard, NULL, i, NULL);
  }
  for (i = 0; i < NFOTRS; ++i) {
    thread_fork_or_panic("elf", elf, NULL, i, NULL);
  }
  for (i = 0; i < NFOTRS; ++i) {
    thread_fork_or_panic("dwarf", dwarf, NULL, i, NULL);
  }
  for (i = 0, n = NFOTRS * MEN_PER_FOTR; i < n; ++i) {
    thread_fork_or_panic("man", man, NULL, i, NULL);
  }
  for (i = 0, n = NFOTRS * HOBBITS_PER_FOTR; i < n; ++i) {
    thread_fork_or_panic("hobbit", hobbit, NULL, i, NULL);
  }

  // Wait on all the threads; clean up
  P(driver_sem);

  for (i=0; i<NFOTRS; i++){
    lock_destroy(fs[i].fellowship_lk);
    lock_destroy(fs[i].cv_lk);
    cv_destroy(fs[i].ready);
    for(j=0; j<9; j++)
      kfree(fs[i].names[j]);
  }

  sem_destroy(print_lock);
  lock_destroy(cleanup_lock);
  sem_destroy(driver_sem);
  kfree(fs);

  return 0;
}
