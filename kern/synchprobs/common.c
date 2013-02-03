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
 * Common synch problem driver code.
 */

#include <types.h>
#include <lib.h>
#include <thread.h>

#include "common.h"

void
thread_fork_or_panic(const char *name,
	    void (*entrypoint)(void *data1, unsigned long data2),
	    void *data1, unsigned long data2,
	    struct thread **ret)
{
  int r;

  r = thread_fork(name, entrypoint, data1, data2, ret);
  if (r) {
    panic("thread_fork: %s\n", strerror(r));
  }
}

///////////////////////////////////////////////////////////////////////////////
//
//  Names for identifying synch problem threads.
//
//  Sure, we could use numbers, but where's the fun without some flavor text?
//

/**
 * List of hobbitses.  My preciousss.
 *
 * There are exactly 39 known Bagginses.  Who knew?
 */
const char *hobbitses[NFOTRS * HOBBITS_PER_FOTR] = {
  "Angelica Baggins",
  "Balbo Baggins",
  "Belba (Baggins) Bolger",
  "Belladonna (Took) Baggins",
  "Berylla (Boffin) Baggins",
  "Bilbo Baggins",
  "Bingo Baggins",
  "Bungo Baggins",
  "Chica (Chubb) Baggins",
  "Daisy (Baggins) Boffin",

  "Dora Baggins",
  "Drogo Baggins",
  "Dudo Baggins",
  "Falco Chubb-Baggins",
  "Fosco Baggins",
  "Frodo Baggins",
  "Gilly (Brownlock) Baggins",
  "Largo Baggins",
  "Laura (Grubb) Baggins",
  "Lily (Baggins) Goodbody",

  "Linda (Baggins) Proudfoot",
  "Lobelia (Bracegirdle) Sackville-Baggins",
  "Longo Baggins",
  "Lotho Sackville-Baggins",
  "Mimosa (Bunce) Baggins",
  "Mungo Baggins",
  "Otho Sackville-Baggins",
  "Pansy (Baggins) Bolger",
  "Peony (Baggins) Burrows",
  "Polo Baggins",

  "Ponto Baggins",
  "Porto Baggins",
  "Posco Baggins",
  "Poppy (Chubb-Baggins) Bolger",
  "Primula (Brandybuck) Baggins",
  "Prisca (Baggins) Bolger",
  "Rosa (Baggins) Took",
  "Ruby (Bolger) Baggins",
  "Tanta (Hornblower) Baggins",
  "Samwise Gamgee", // The only Hobbit worth anything.
};

/**
 * List of elves.
 */
const char *eldar[NFOTRS] = {
  "Arwen",
  "Celeborn",
  "Celebrimbor",
  "Elrond",
  "Feanor",
  "Finwe",
  "Galadriel",
  "Gil-Galad",
  "Legolas",
  "Thranduil",
};

/**
 * List of dwarves.
 */
const char *khazad[NFOTRS] = {
  "Durin",
  "Gimli",
  "Fili",
  "Kili",
  "Bifur",
  "Bofur",
  "Bombur",
  "Dwalin",
  "Balin",
  "Thorin Oakenshield",
};

/**
 * List of wizards.
 *
 * Wizards are magical so they appear multiple times.  With different names.
 */
const char *istari[NFOTRS] = {
  "Curunir",
  "Saruman of Many Colours",
  "Olorin",
  "Gandalf the Grey",
  "Gandalf the White",
  "Aiwendil",
  "Radagast the Brown",
  "Alatar",
  "Pallando",
  "Margo",
};

/**
 * List of men.
 */
const char *menfolk[NFOTRS * MEN_PER_FOTR] = {
  "Amandil",
  "Ar-Pharazon",
  "Aragorn II Elessar",
  "Bard the Bowman",
  "Boromir",
  "Carl",
  "Denethor II",
  "Earendil",
  "Elendil",
  "Elros",

  "Eomer",
  "Eowyn",
  "Faramir",
  "Hyarmendacil II",
  "Isildur",
  "Max",
  "Ondoher",
  "Rob",
  "Theoden",
  "Vinitharya",
};
