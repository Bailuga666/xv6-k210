#ifndef __TIMER_H
#define __TIMER_H

#include "types.h"
#include "spinlock.h"

extern struct spinlock tickslock;
extern uint ticks;

void timerinit();
void set_next_timeout();
void timer_tick();
struct tms {
    long tms_utime; 
    long tms_stime; 
    long tms_cutime; 
    long tms_cstime; 
};
// man page复制
#endif
