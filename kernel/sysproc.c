
#include "include/types.h"
#include "include/riscv.h"
#include "include/param.h"
#include "include/memlayout.h"
#include "include/spinlock.h"
#include "include/proc.h"
#include "include/syscall.h"
#include "include/timer.h"
#include "include/kalloc.h"
#include "include/string.h"
#include "include/printf.h"
#include "include/vm.h"
#include "include/syscall.h"

extern int exec(char *path, char **argv);

uint64
sys_exec(void)
{
  char path[FAT32_MAX_PATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, FAT32_MAX_PATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(-1, p);
}

uint64
sys_wait4(void)
{
  int pid;
  int options;
  uint64 status;

  if(argint(0, &pid) < 0 || argaddr(1, &status) < 0 || argint(2, &options) < 0)
    return -1;
  return wait4(pid, status, options);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  int mask;
  if(argint(0, &mask) < 0) {
    return -1;
  }
  myproc()->tmask = mask;
  return 0;
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

uint64 sys_uname(void) {
  struct uname_info {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
  };

  // 这个数据当前是准备在内核的栈内存中的
  struct uname_info info = {
    "xv6",
    "bailuga",
    "1.0.0",
    "1.0.0",
    "bailuga",
    "bailuga"
  };
  // 瞎写一堆
  if (safe_copy(0, (char *)&info, sizeof(info)) < 0) {
    return -1;
  }
  // 保证成功复制
  return 0;
}
struct timespec {
    uint64 tv_sec;
    uint64 tv_usec;
};

uint64 sys_gettimeofday(void) {
  struct timespec ts;
  // 一个结构体
  uint64 htick = r_time(); 
  // 取硬件ticks

  ts.tv_sec = htick / CLOCK_FREQ;
   // 换算成秒
  ts.tv_usec = (htick % CLOCK_FREQ) * 1000000 / CLOCK_FREQ; 
  // 换算成微秒

  if (safe_copy(0, (char *)&ts, sizeof(ts)) < 0) {
    return -1;
  }
  // 复制
  return 0;
}

uint64
sys_nanosleep(void)
{
  uint64 duration, rem;
  // 两个参数，存放睡眠时间，返回剩余时间的地址
  struct timespec req_tv; 
  // 要求的结构体
  if (argaddr(0, &duration) < 0 || argaddr(1, &rem) < 0) {
    return -1;
  }
  // 获取两个参数
  if (copyin2((char *)&req_tv, duration, sizeof(struct timespec)) < 0) {
    return -1;
  }
  // 赋值
  uint64 target_ticks = req_tv.tv_sec * TICKS_PER_SECOND + req_tv.tv_usec * TICKS_PER_SECOND / 1000000;
  // 计算需要经过的ticks
  acquire(&tickslock);
  // 加锁
  uint64 ticks_now;
  // 起始时间
  ticks_now = ticks;
  // 记录起始时间
  while (ticks - ticks_now < target_ticks) {
    // 时间未到
    if (myproc()->killed) {
      // 如果被提前唤醒了
      if (rem != NULL) {
        // 如果有存放剩余时间的地址
        uint64 used_ticks = ticks - ticks_now;
        uint64 rem_ticks = (target_ticks > used_ticks) ? (target_ticks - used_ticks) : 0;
        // 计算剩余
        struct timespec rem_tv;
        rem_tv.tv_sec = rem_ticks / TICKS_PER_SECOND;
        rem_tv.tv_usec = (rem_ticks % TICKS_PER_SECOND) * 1000000 / TICKS_PER_SECOND;
        // 换算成结构体
        if (copyout2(rem, (char *)&rem_tv, sizeof(struct timespec)) < 0) {
          release(&tickslock);
          return -1;
        }
        // 复制到目标地址
      }
      release(&tickslock);
      // 解锁
      return -1;
      // 参数不对，-1
    }
    sleep(&ticks, &tickslock);
    // 睡眠，等待ticks变化
  }
  release(&tickslock);
  // 解锁
  return 0;

}

uint64
sys_clone(void)
{
  return clone();
}

uint64
sys_sched_yield(void) {
  yield();
  return 0;
}

uint64
sys_getppid(void)
{
  return myproc()->parent->pid;
}