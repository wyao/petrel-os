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

/**
 * Driver code for The Fellowship of the Ring synch problem.
 */

#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>

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


///////////////////////////////////////////////////////////////////////////////
//
//  Driver code.
//
//  TODO: Implement all the thread entrypoints!
//

static void
wizard(void *p, unsigned long which)
{
  (void)p;

  // TODO: Join a fellowship.
  nameof_istari(which);
}

static void
man(void *p, unsigned long which)
{
  (void)p;

  // TODO: Join a fellowship.
  nameof_menfolk(which);
}

static void
elf(void *p, unsigned long which)
{
  (void)p;

  // TODO: Join a fellowship.
  nameof_eldar(which);
}

static void
dwarf(void *p, unsigned long which)
{
  (void)p;

  // TODO: Join a fellowship.
  nameof_khazad(which);
}

static void
hobbit(void *p, unsigned long which)
{
  (void)p;

  // TODO: Join a fellowship.
  nameof_hobbitses(which);
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
  int i, n;

  (void)nargs;
  (void)args;

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

  return 0;
}
