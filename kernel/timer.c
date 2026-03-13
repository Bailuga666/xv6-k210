// Timer Interrupt handler


#include "include/types.h"
#include "include/param.h"
#include "include/riscv.h"
#include "include/sbi.h"
#include "include/spinlock.h"
#include "include/timer.h"
#include "include/printf.h"
#include "include/proc.h"
#include "include/vm.h"
#include "include/syscall.h"
struct spinlock tickslock;
uint ticks;

void timerinit() {
    initlock(&tickslock, "time");
    #ifdef DEBUG
    printf("timerinit\n");
    #endif
}

void
set_next_timeout() {
    // There is a very strange bug,
    // if comment the `printf` line below
    // the timer will not work.

    // this bug seems to disappear automatically
    // printf("");
    sbi_set_timer(r_time() + INTERVAL);
}

void timer_tick() {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
    set_next_timeout();
}

static int
safe_copy(uint64 arg_index, char *src, uint64 size)
{
  uint64 dest_addr;
  if (argaddr(arg_index, &dest_addr) < 0)
    return -1;
  if (copyout2(dest_addr, src, size) < 0)
    return -1;
  return 0;
}

uint64 sys_times(void) {
  struct tms tms;
    // 创建一个结构体
  acquire(&tickslock);
//   加锁
  tms.tms_utime = tms.tms_stime = tms.tms_cutime = tms.tms_cstime = ticks;
  release(&tickslock);
//   释放锁

  if (safe_copy(0, (char *)&tms, sizeof(tms)) < 0) {
    return -1;
  }
//   如果复制失败了，-1
  return 0;
}