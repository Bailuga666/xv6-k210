#ifndef __VM_H 
#define __VM_H 

#include "types.h"
#include "riscv.h"

void            kvminit(void);
void            kvminithart(void);
uint64          kvmpa(uint64);
void            kvmmap(uint64, uint64, uint64, int);
int             mappages(pagetable_t, uint64, uint64, uint64, int);
pagetable_t     uvmcreate(void);
// void            uvminit(pagetable_t, uchar *, uint);
void            uvminit(pagetable_t, pagetable_t, uchar *, uint);
uint64          uvmalloc(pagetable_t, pagetable_t, uint64, uint64);
uint64          uvmdealloc(pagetable_t, pagetable_t, uint64, uint64);
// int             uvmcopy(pagetable_t, pagetable_t, uint64);
int             uvmcopy(pagetable_t, pagetable_t, pagetable_t, uint64);
void            uvmfree(pagetable_t, uint64);
// void            uvmunmap(pagetable_t, uint64, uint64, int);
void            vmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
uint64          walkaddr(pagetable_t, uint64);
int             copyout(pagetable_t, uint64, char *, uint64);
int             copyin(pagetable_t, char *, uint64, uint64);
int             copyinstr(pagetable_t, char *, uint64, uint64);
pagetable_t     proc_kpagetable(void);
void            kvmfreeusr(pagetable_t kpt);
void            kvmfree(pagetable_t kpagetable, int stack_free);
uint64          kwalkaddr(pagetable_t pagetable, uint64 va);
int             copyout2(uint64 dstva, char *src, uint64 len);
int             copyin2(char *dst, uint64 srcva, uint64 len);
int             copyinstr2(char *dst, uint64 srcva, uint64 max);

#define NVMA 32

#define PROT_READ       (1 << 0)
#define PROT_WRITE      (1 << 1)
#define PROT_EXEC       (1 << 2)

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

struct vma {
    int valid;              // 有效
    uint64 start;           // 起始点
    uint64 end;             // 结束点
    int prot;               // 访问权限 (PROT_READ, PROT_WRITE, PROT_EXEC)
    int flags;              // 映射标志 (MAP_SHARED, MAP_PRIVATE, MAP_ANONYMOUS)
    struct file* vm_file;   // 被映射的 file 结构体，匿名映射时为 NULL
    uint64 offset;          // 偏移量
};
#endif 
struct proc;
// 写回与释放
void            vma_writeback(struct proc* p, struct vma* v);
void            vma_free(struct proc* p);
void            vmprint(pagetable_t pagetable);
// 寻找一个够长的虚拟地址空间
uint64          mmap_find_addr(struct proc* p, uint64 len);