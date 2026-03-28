//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//


#include "include/types.h"
#include "include/riscv.h"
#include "include/param.h"
#include "include/stat.h"
#include "include/spinlock.h"
#include "include/proc.h"
#include "include/sleeplock.h"
#include "include/file.h"
#include "include/pipe.h"
#include "include/fcntl.h"
#include "include/fat32.h"
#include "include/syscall.h"
#include "include/string.h"
#include "include/printf.h"
#include "include/vm.h"


// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == NULL)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

static struct dirent*
create(char *path, short type, int mode)
{
  struct dirent *ep, *dp;
  char name[FAT32_MAX_FILENAME + 1];

  if((dp = enameparent(path, name)) == NULL)
    return NULL;

  if (type == T_DIR) {
    mode = ATTR_DIRECTORY;
  } else if (mode & O_RDONLY) {
    mode = ATTR_READ_ONLY;
  } else {
    mode = 0;  
  }

  elock(dp);
  if ((ep = ealloc(dp, name, mode)) == NULL) {
    eunlock(dp);
    eput(dp);
    return NULL;
  }
  
  if ((type == T_DIR && !(ep->attribute & ATTR_DIRECTORY)) ||
      (type == T_FILE && (ep->attribute & ATTR_DIRECTORY))) {
    eunlock(dp);
    eput(ep);
    eput(dp);
    return NULL;
  }

  eunlock(dp);
  eput(dp);

  elock(ep);
  return ep;
}

uint64
sys_open(void)
{
  char path[FAT32_MAX_PATH];
  int fd, omode;
  struct file *f;
  struct dirent *ep;

  if(argstr(0, path, FAT32_MAX_PATH) < 0 || argint(1, &omode) < 0)
    return -1;

  if(omode & O_CREATE){
    ep = create(path, T_FILE, omode);
    if(ep == NULL){
      return -1;
    }
  } else {
    if((ep = ename(path)) == NULL){
      return -1;
    }
    elock(ep);
    if((ep->attribute & ATTR_DIRECTORY) && (omode & (O_WRONLY | O_RDWR))){
      eunlock(ep);
      eput(ep);
      return -1;
    }
  }

  if((f = filealloc()) == NULL || (fd = fdalloc(f)) < 0){
    if (f) {
      fileclose(f);
    }
    eunlock(ep);
    eput(ep);
    return -1;
  }

  if(!(ep->attribute & ATTR_DIRECTORY) && (omode & O_TRUNC)){
    etrunc(ep);
  }

  f->type = FD_ENTRY;
  f->off = (omode & O_APPEND) ? ep->file_size : 0;
  f->ep = ep;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  eunlock(ep);

  return fd;
}
static int get_abspath(struct dirent *de, char *path_buf, int buf_size)
{
  // chain 用来暂存从当前目录到根目录这条链，后面会反向拼接成绝对路径。
  struct dirent *chain[64];
  int depth = 0;
  int i;
  int used;

  // 输出缓冲区至少要能放下 "/" 和字符串结束符。
  if(path_buf == NULL || buf_size < 2)
    return -1;

  // 从当前目录一路向 parent 回溯，先收集路径节点，避免递归实现带来的栈层级波动。
  while(de != NULL && de->parent != NULL){
    // 防止极端深目录导致本地数组越界。
    if(depth >= 64)
      return -1;
    chain[depth++] = de;
    de = de->parent;
  }

  // 绝对路径统一从根开始构造。
  path_buf[0] = '/';
  path_buf[1] = '\0';
  used = 1;

  // 反向遍历链表：先父后子，最终得到类似 /a/b/c 的路径。
  for(i = depth - 1; i >= 0; i--){
    int name_len = strlen(chain[i]->filename);
    // used>1 说明当前缓冲区不是根路径，需要额外拼接 '/' 分隔符。
    int need_slash = (used > 1);

    // 预先检查剩余空间，避免任何一次 memmove 写越界。
    if(used + need_slash + name_len + 1 > buf_size)
      return -1;

    if(need_slash)
      path_buf[used++] = '/';
    // 目录名使用定长拷贝，末尾由下方统一补 '\0'。
    memmove(path_buf + used, chain[i]->filename, name_len);
    used += name_len;
    path_buf[used] = '\0';
  }

  return 0;
}

static int resolve_base_dir(int fd, struct dirent **out)
{
  // f 对应进程打开文件表中的某个打开实例，用来从 fd 追溯到目录项 ep。
  struct file *f;
 
  // 输出参数必须有效，否则调用者无法拿到解析基准目录。
  if(out == NULL)
    return -1;
  // AT_FDCWD 表示以当前工作目录为解析基准，直接返回即可。
  if(fd == AT_FDCWD){
    *out = myproc()->cwd;
    return 0;
  }

  // 普通 fd 必须处于当前进程 ofile[] 的合法下标范围。
  if(fd < 0 || fd >= NOFILE)
    return -1;
  // 非 AT_FDCWD 的 fd 需要验证合法性，并且必须是一个目录类型的文件，才能作为解析基准。
  f = myproc()->ofile[fd];
  // f 为空表示该 fd 未打开；f->ep 为空通常表示设备/管道等非目录项对象。
  if(f == NULL || f->ep == NULL || !(f->ep->attribute & ATTR_DIRECTORY))
    return -1;

  // 走到这里说明 fd 对应的是一个可作为路径解析起点的目录。
  *out = f->ep;
  return 0;
}

int get_path(char *path, int fd)
{
  // base_de 表示解析相对路径时的“起点目录”。
  struct dirent *base_de;
  char base_path[FAT32_MAX_PATH];
  // rel_path 是相对后缀的副本，避免后续覆盖 path 时破坏原始片段。
  char rel_path[FAT32_MAX_PATH];
  char *rel;
  int base_len;
  int rel_len;
  int need_slash;

  // path 为输入/输出参数：输入可为相对路径，输出会被改写成绝对路径。
  if(path == NULL)
    return -1;

  // 已是绝对路径则无需转换，直接复用原字符串。
  if(path[0] == '/')
    return 0;

  // 支持 "./xxx" 形式：仅去掉前缀，不改变真实路径语义。
  rel = path;
  if(rel[0] == '.' && rel[1] == '/')
    rel += 2;

  // 先缓存相对后缀，避免下面写入 base_path 时把 rel 指向内容覆盖掉。
  safestrcpy(rel_path, rel, sizeof(rel_path));

  // 先确定解析基准目录，再把该目录转换成绝对路径字符串。
  if(resolve_base_dir(fd, &base_de) < 0)
    return -1;
  if(get_abspath(base_de, base_path, FAT32_MAX_PATH) < 0)
    return -1;

  // 计算拼接后长度：base + 可选'/' + rel + '\0'。
  base_len = strlen(base_path);
  rel_len = strlen(rel_path);
  need_slash = (base_len > 1 && rel_len > 0);

  // 再次做总长度保护，保证最终 path 缓冲区可容纳完整结果。
  if(base_len + need_slash + rel_len + 1 > FAT32_MAX_PATH)
    return -1;

  // 先写入基路径，再按需补 '/'，最后拼接相对后缀。
  memmove(path, base_path, base_len);
  if(need_slash)
    path[base_len++] = '/';
  memmove(path + base_len, rel_path, rel_len);
  // 统一补字符串结束符，保证调用方拿到合法 C 字符串。
  path[base_len + rel_len] = '\0';

  return 0;
}



uint64
sys_openat(void)
{
  // path 是内核侧缓冲区：先接收用户参数，再被 get_path 规范化为绝对路径。
  char path[FAT32_MAX_PATH];
  int dirfd, flags, mode, fd;
  // f 是打开文件实例；ep 是 FAT32 的目录项对象。
  struct file *f = 0;
  struct dirent *ep = 0;

  // 读取 openat(dirfd, pathname, flags, mode) 四个参数。
  if(argint(0, &dirfd) < 0 ||
     argstr(1, path, FAT32_MAX_PATH) < 0 ||
     argint(2, &flags) < 0 ||
     argint(3, &mode) < 0)
    return -1;

  // 空路径在语义上无效，直接报错。
  if(path[0] == '\0')
    return -1;

  // openat 的关键步骤：相对路径按 dirfd 解析，统一转换为绝对路径。
  if(get_path(path, dirfd) < 0)
    return -1;

  // O_CREATE 分支会在父目录下创建目标文件并返回已加锁的 ep。
  if(flags & O_CREATE){
    ep = create(path, T_FILE, mode);
    if(ep == NULL)
      return -1;
  } else {
    // 非创建分支只做查找，失败说明文件不存在。
    ep = ename(path);
    if(ep == NULL)
      return -1;

    // 查到后加锁，避免属性检查与后续操作被并发修改。
    elock(ep);
    // 目录禁止按写方式打开，防止把目录当普通文件写入。
    if((ep->attribute & ATTR_DIRECTORY) && (flags & (O_WRONLY | O_RDWR))){
      eunlock(ep);
      eput(ep);
      return -1;
    }
  }

  // 先分配全局 file 实例，再在当前进程 ofile[] 里分配一个 fd 槽位。
  f = filealloc();
  if(f == NULL){
    if(ep)
      eunlock(ep);
    if(ep)
      eput(ep);
    return -1;
  }
  fd = fdalloc(f);
  if(fd < 0){
    fileclose(f);
    if(ep)
      eunlock(ep);
    if(ep)
      eput(ep);
    return -1;
  }

  // 仅普通文件支持 O_TRUNC，目录不允许做截断。
  if(!(ep->attribute & ATTR_DIRECTORY) && (flags & O_TRUNC))
    etrunc(ep);

  // 初始化打开实例，后续 read/write 都通过这个 file 结构驱动。
  f->type = FD_ENTRY;
  f->off = (flags & O_APPEND) ? ep->file_size : 0;
  f->ep = ep;
  f->readable = !(flags & O_WRONLY);
  f->writable = (flags & O_WRONLY) || (flags & O_RDWR);

  // 正常路径释放目录项锁并返回新 fd。
  eunlock(ep);
  return fd;
}




















uint64
sys_mkdir(void)
{
  char path[FAT32_MAX_PATH];
  struct dirent *ep;

  if(argstr(0, path, FAT32_MAX_PATH) < 0 || (ep = create(path, T_DIR, 0)) == 0){
    return -1;
  }
  eunlock(ep);
  eput(ep);
  return 0;
}

uint64
sys_chdir(void)
{
  char path[FAT32_MAX_PATH];
  struct dirent *ep;
  struct proc *p = myproc();
  
  if(argstr(0, path, FAT32_MAX_PATH) < 0 || (ep = ename(path)) == NULL){
    return -1;
  }
  elock(ep);
  if(!(ep->attribute & ATTR_DIRECTORY)){
    eunlock(ep);
    eput(ep);
    return -1;
  }
  eunlock(ep);
  eput(p->cwd);
  p->cwd = ep;
  return 0;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  // if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
  //    copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
  if(copyout2(fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout2(fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// To open console device.
uint64
sys_dev(void)
{
  int fd, omode;
  int major, minor;
  struct file *f;

  if(argint(0, &omode) < 0 || argint(1, &major) < 0 || argint(2, &minor) < 0){
    return -1;
  }

  if(omode & O_CREATE){
    panic("dev file on FAT");
  }

  if(major < 0 || major >= NDEV)
    return -1;

  if((f = filealloc()) == NULL || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    return -1;
  }

  f->type = FD_DEVICE;
  f->off = 0;
  f->ep = 0;
  f->major = major;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  return fd;
}

// To support ls command
uint64
sys_readdir(void)
{
  struct file *f;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argaddr(1, &p) < 0)
    return -1;
  return dirnext(f, p);
}

// get absolute cwd string
uint64
sys_getcwd(void) {
  uint64 addr;
  int size;
  // 地址和缓冲区大小
  if (argaddr(0, &addr) < 0 || argint(1, &size) < 0)
    return NULL;
  // 确保正确读入参数
  struct dirent* de = myproc()->cwd;
  char path[size];
  // 不放在最大的了，只给他缓冲区大小的路径长度
  char* s = path + sizeof(path) - 1;
  *s = '\0';
  // 先给最后写一个/0
  if (de->parent == NULL) {
    s--;
    *s = '/';
  }
  // 如果根目录，直接返回/
  else {
    while (de->parent) {
      // 如果有父目录
      int len = strlen(de->filename);
      s -= len;
      if (s < path)
        return NULL;
      // 反向写父目录地址，但是检查会不会出界
      memmove(s, de->filename, len);
      // 写入
      s--;
      if (s < path)
        return NULL;
      *s = '/';
      // 检查会不会出界，不出界就补一个/
      de = de->parent;
    }
  }

  memmove(path, s, strlen(s) + 1);
  // 移动到开头
  if (copyout2(addr, path, strlen(path) + 1) < 0)
    return NULL;
  // 如果copy出问题了也null
  return addr;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct dirent *dp)
{
  struct dirent ep;
  int count;
  int ret;
  ep.valid = 0;
  ret = enext(dp, &ep, 2 * 32, &count);   // skip the "." and ".."
  return ret == -1;
}

uint64
sys_remove(void)
{
  char path[FAT32_MAX_PATH];
  struct dirent *ep;
  int len;
  if((len = argstr(0, path, FAT32_MAX_PATH)) <= 0)
    return -1;

  char *s = path + len - 1;
  while (s >= path && *s == '/') {
    s--;
  }
  if (s >= path && *s == '.' && (s == path || *--s == '/')) {
    return -1;
  }
  
  if((ep = ename(path)) == NULL){
    return -1;
  }
  elock(ep);
  if((ep->attribute & ATTR_DIRECTORY) && !isdirempty(ep)){
      eunlock(ep);
      eput(ep);
      return -1;
  }
  elock(ep->parent);      // Will this lead to deadlock?
  eremove(ep);
  eunlock(ep->parent);
  eunlock(ep);
  eput(ep);

  return 0;
}

// Must hold too many locks at a time! It's possible to raise a deadlock.
// Because this op takes some steps, we can't promise
uint64
sys_rename(void)
{
  char old[FAT32_MAX_PATH], new[FAT32_MAX_PATH];
  if (argstr(0, old, FAT32_MAX_PATH) < 0 || argstr(1, new, FAT32_MAX_PATH) < 0) {
      return -1;
  }

  struct dirent *src = NULL, *dst = NULL, *pdst = NULL;
  int srclock = 0;
  char *name;
  if ((src = ename(old)) == NULL || (pdst = enameparent(new, old)) == NULL
      || (name = formatname(old)) == NULL) {
    goto fail;          // src doesn't exist || dst parent doesn't exist || illegal new name
  }
  for (struct dirent *ep = pdst; ep != NULL; ep = ep->parent) {
    if (ep == src) {    // In what universe can we move a directory into its child?
      goto fail;
    }
  }

  uint off;
  elock(src);     // must hold child's lock before acquiring parent's, because we do so in other similar cases
  srclock = 1;
  elock(pdst);
  dst = dirlookup(pdst, name, &off);
  if (dst != NULL) {
    eunlock(pdst);
    if (src == dst) {
      goto fail;
    } else if (src->attribute & dst->attribute & ATTR_DIRECTORY) {
      elock(dst);
      if (!isdirempty(dst)) {    // it's ok to overwrite an empty dir
        eunlock(dst);
        goto fail;
      }
      elock(pdst);
    } else {                    // src is not a dir || dst exists and is not an dir
      goto fail;
    }
  }

  if (dst) {
    eremove(dst);
    eunlock(dst);
  }
  memmove(src->filename, name, FAT32_MAX_FILENAME);
  emake(pdst, src, off);
  if (src->parent != pdst) {
    eunlock(pdst);
    elock(src->parent);
  }
  eremove(src);
  eunlock(src->parent);
  struct dirent *psrc = src->parent;  // src must not be root, or it won't pass the for-loop test
  src->parent = edup(pdst);
  src->off = off;
  src->valid = 1;
  eunlock(src);

  eput(psrc);
  if (dst) {
    eput(dst);
  }
  eput(pdst);
  eput(src);

  return 0;

fail:
  if (srclock)
    eunlock(src);
  if (dst)
    eput(dst);
  if (pdst)
    eput(pdst);
  if (src)
    eput(src);
  return -1;
}
