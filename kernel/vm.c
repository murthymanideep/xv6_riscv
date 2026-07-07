#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "raid.h"

struct swapslot{
  int in_use;
  struct proc *p;
};
extern struct swapslot swaptable[MAX_SWAP_PAGES];
extern struct spinlock swaplock;

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory or swap slots.
void
uvmunmap(pagetable_t pagetable,uint64 va,uint64 npages,int do_free)
{
  uint64 a;
  pte_t *pte;
  if((va%PGSIZE)!=0)
    panic("uvmunmap: not aligned");

  for(a=va;a<va+npages*PGSIZE;a+=PGSIZE){
    if((pte=walk(pagetable,a,0))==0){
      continue; 
    }
    
    if((*pte & PTE_V)==0 && (*pte & PTE_S)==0){  
      continue;
    }

    if(do_free){
      if(*pte & PTE_S){
        int s_idx=(*pte>>10);
        *pte=0;
        free_swap_slot(s_idx);
      } 
      else if(*pte & PTE_V){
        uint64 pa=PTE2PA(*pte);
        *pte=0;
        sfence_vma();
        kfree_user(pa);
      }
    } 
    else{
      *pte=0;
    }
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  struct proc* p=myproc();

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc_user(a,p);
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree_user((uint64)mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the physical memory.
// Handles both resident (PTE_V) and swapped-out (PTE_S) pages.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
/*int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz, struct proc *np)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;
  uint64 pte_val;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   
      
    pte_val = *pte;
    // Skip if it's neither valid nor swapped out
    if((pte_val & PTE_V) == 0 && (pte_val & PTE_S) == 0)
      continue;   

    // Allocate frame for the child. 
    // Note: This might trigger an eviction, changing the state of the parent's PTE!
    if((mem = (char*)kalloc_user(i, np)) == 0)
      goto err;

    // Re-read PTE because kalloc_user might have swapped this very page out
    pte_val = *pte; 
    flags = PTE_FLAGS(pte_val);

    if(pte_val & PTE_V){
      // Parent page is in RAM. Copy it normally.
      pa = PTE2PA(pte_val);
      memmove(mem, (char*)pa, PGSIZE);
    } 
    else if(pte_val & PTE_S){
      // Parent page is on disk. 
      int s_idx = (pte_val >> 10);
      
      if(s_idx < 0){ // Adjust MAX_SWAP_PAGES check if you have a macro for it
        panic("uvmcopy: invalid swap index");
      }
      
      // Read directly from disk into the child's new physical memory.
      // (Change 'raid_read_page' to whatever your exact read function is named if different)
      raid_read_page(s_idx, (uint64)mem,get_raid_mode()); 
      
      // The child's new page is in RAM, so clear the swap bit and set valid bit
      flags = (flags & ~PTE_S) | PTE_V;
    } 
    else{
      // Fallback safety catch
      kfree_user((uint64)mem);
      continue;
    }

    // Map the new physical page into the child's page table
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree_user((uint64)mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}*/


int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz, struct proc *np)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;
  uint64 pte_val;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   
      
    pte_val = *pte;
    if((pte_val & PTE_V) == 0 && (pte_val & PTE_S) == 0)
      continue;   

    if(pte_val & PTE_S){
      int parent_s_idx = (pte_val >> 10);
      int child_s_idx = -1;

      // Allocate a completely new swap slot for the child
      acquire(&swaplock);
      for(int j = 0; j < MAX_SWAP_PAGES; j++){
        if(!swaptable[j].in_use){
          child_s_idx = j;
          swaptable[child_s_idx].in_use = 1;
          swaptable[child_s_idx].p = np;
          break;
        }
      }
      release(&swaplock);

      if(child_s_idx == -1) goto err; // Swap space full

      // Allocate a temporary bounce buffer using pure kalloc()
      void *bounce_buffer = kalloc();
      if(bounce_buffer == 0){
        free_swap_slot(child_s_idx);
        goto err;
      }

      raid_read_page(parent_s_idx, (uint64)bounce_buffer, get_raid_mode());
      raid_write_page(child_s_idx, (uint64)bounce_buffer, get_raid_mode());

      kfree(bounce_buffer);
      pte_t *child_pte = walk(new, i, 1);
      if(child_pte == 0){
        free_swap_slot(child_s_idx);
        goto err;
      }

      flags = PTE_FLAGS(pte_val);
      *child_pte = ((uint64)child_s_idx << 10) | (flags & ~PTE_V) | PTE_S;
      
      np->pages_swapped_out++;
    } 
    else if(pte_val & PTE_V) {
      if((mem = (char*)kalloc_user(i, np)) == 0)
        goto err;
      pte_val = *pte; 
      flags = PTE_FLAGS(pte_val);

      if(pte_val & PTE_V){
        pa = PTE2PA(pte_val);
        memmove(mem, (char*)pa, PGSIZE);
      } 
      else if(pte_val & PTE_S){
        int s_idx = (pte_val >> 10);
        raid_read_page(s_idx, (uint64)mem, get_raid_mode());
        flags = (flags & ~PTE_S) | PTE_V;
      }
      if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
        kfree_user((uint64)mem);
        goto err;
      }
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64 vmfault(pagetable_t pagetable,uint64 va,int read){
  struct proc *p=myproc();
  if(va>=p->sz){
    return 0;
  }
  va=PGROUNDDOWN(va);
  if(ismapped(pagetable,va)){
    p->page_faults++;
    return 0;
  }
  pte_t *pte=walk(pagetable,va,0);
  uint64 pa=(uint64)kalloc_user(va,p);
  if(pa==0){
    return 0;
  }

  if(pte && (*pte&PTE_S)){
    swap_in(p,va,pa);
    p->page_faults++; // swap fault
    return pa;
  }

  memset((void*)pa,0,PGSIZE);
  if(mappages(p->pagetable,va,PGSIZE,pa,PTE_W|PTE_U|PTE_R)!=0){
    kfree_user(pa);
    return 0;
  }

  p->page_faults++; // lazy allocation fault
  return pa;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}
