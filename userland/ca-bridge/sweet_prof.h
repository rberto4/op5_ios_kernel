/* (op5) Sampling profiler of the drawing thread (sweet_prof.c): on when /usr/local/lib/glprof exists. */
#ifndef SWEET_PROF_H
#define SWEET_PROF_H
void sweet_prof_start(void);    /* once, at load */
void sweet_prof_thread(void);   /* from the drawing thread: it is the one sampled */
#endif
