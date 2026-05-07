// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define NUM_FRAMES 64
#define MAX_SWAP_PAGES 32000

// frame table entry
struct frame{
  int in_use;
  struct proc* p;
  uint64 va; // virtual address
  uint64 pa; // physical address
  int ref;
};
// Global frame table to track physical pages
struct frame frametable[NUM_FRAMES];
// Lock to acess frame table
struct spinlock framelock;
// Clock hand
int clock_hand=0;
// Swap slot
struct swapslot{
  int in_use;
  struct proc* p;
};
// Swap table
struct swapslot swaptable[MAX_SWAP_PAGES];
// Swap space
char swapspace[MAX_SWAP_PAGES][PGSIZE];
struct spinlock swaplock;

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&framelock, "frametable");
  initlock(&swaplock, "swaptable");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}


// Swap out the victim page
void swap_out(struct frame* f){
  int s_idx=-1; // swap table index

  // Find a swap slot
  acquire(&swaplock);
  for(int i=0;i<MAX_SWAP_PAGES;i++){
    if(!swaptable[i].in_use){
      swaptable[i].in_use=1;
      swaptable[i].p=f->p;
      s_idx=i;
    }
  }
  release(&swaplock);
  if(s_idx<0){
    panic("Swap space is full");
  }

  pte_t *pte=walk(f->p->pagetable,f->va,0);
  int flags=PTE_FLAGS(*pte);
  flags=(flags&PTE_V)&PTE_S;
  *pte=((uint64)s_idx<<10)|flags;

  // Flush TLB
  sfence_vma();
  // Copy data into the swap space
  memmove(swapspace[s_idx],(char*)f->pa,PGSIZE);

  f->p->resident_pages--;
  f->p->pages_evicted++;
  f->p->pages_swapped_out++;
}


// Bring the page from swap space to the memory
void swap_in(struct proc* p,uint64 va,uint64 new_pa){
  // Page table entry
  pte_t *pte=walk(p->pagetable,va,0);
  if(pte==0){
    panic("swap_in_page: error");
  }

  int s_idx=(*pte>>10);
  if(s_idx<0 || s_idx>=MAX_SWAP_PAGES){
    panic("swap_in_page: invalid index");
  }

  memmove((char*)new_pa,swapspace[s_idx],PGSIZE);
  free_swap_slot(s_idx);

  // Update the flags
  int flags=PTE_FLAGS(*pte);
  flags=(flags & ~PTE_S) | PTE_V;
  *pte=PA2PTE(new_pa) | flags;

  p->pages_swapped_in++;
  sfence_vma();
}

// Clock algorithm to find the victim page to evict
// Run the clock algorithm on the lowest priority processes
/* 
 If no process with a lower priority than the current process is found, 
 then page replacement is performed by reclaiming pages from the current process only
*/
int get_victim(){
  int num_frames_scanned=0;
  int low_pri=-1;

  // Find the low priority(high level) process in the frame table
  for(int i=0;i<NUM_FRAMES;i++){
    struct frame* f=&frametable[i];
    if(f->in_use && f->p){
      if(f->p->level>low_pri){
        low_pri=f->p->level;
      }
    }
  }


  while(num_frames_scanned<2*NUM_FRAMES){
    int idx=clock_hand;
    clock_hand=(clock_hand+1)%NUM_FRAMES;
    struct frame* f=&frametable[idx];

    if(f->in_use && f->p && f->p->pagetable){
      // Skip system processes
      if(f->p->pid==1 || f->p->pid==2){
        num_frames_scanned++;
        continue;
      }

      // Check on for lowest priority process
      if(f->p->level!=low_pri){
        num_frames_scanned++;
        continue;
      }

      pte_t* pte=walk(f->p->pagetable,f->va,0);
      if(!pte || !(*pte&PTE_V)){
        num_frames_scanned++;
        continue;
      }
      if(PTE2PA(*pte)!=f->pa){
        num_frames_scanned++;
        continue;
      }

      // Check acessed bit
      if(*pte & PTE_A){
        f->ref=1;
        *pte&=~PTE_A;
        sfence_vma();
      }
      // Second chance logic software ref bit
      if(f->ref==1){
        f->ref=0;
      }
      else{
        return idx;
      }
    }
    num_frames_scanned++;
  }
  panic("get_victim: no victim found");
}

// Allocates memory for the user process
// Implemented because kalloc fails if the memory is full
// Instead we find a victim page and evict it from memory
void* kalloc_user(uint64 va,struct proc* p){
  acquire(&framelock);
  // Find a empty slot in the frame table
  int slot=-1;
  uint64 pa=0;
  for(int i=0;i<NUM_FRAMES;i++){
    if(!frametable[i].in_use){
      slot=i;
      break;
    }
  }

  // If we find a slot
  // Get the physical address of the page from the free list
  if(slot!=-1) {
    void *mem=kalloc(); //gets pa from free list 
    if(mem!=0){
      pa=(uint64)mem;
    } 
    else{
      slot=-1; 
    }
  }

  // Find the victim page
  // Swap out the vicim page
  // get_victim and swap_out are not implemented yet
  if(slot==-1){
    slot=get_victim();
    struct frame *f=&frametable[slot];
    // We have process in frametable so pass the frame table slot
    swap_out(f);
    pa=f->pa;
  }

  // Update the frame table slot
  frametable[slot].pa=pa;
  frametable[slot].va=PGROUNDDOWN(va);
  frametable[slot].p=p;
  frametable[slot].in_use=1;
  frametable[slot].ref=1;

  p->resident_pages++; // Increase resident pages
  release(&framelock);
  return (void*)pa;
}

// Clear frame table entry for given physical address
void kfree_user(uint64 pa){
  acquire(&framelock);
  for(int i=0;i<NUM_FRAMES;i++){
    if(frametable[i].in_use && frametable[i].pa==pa){
      frametable[i].in_use=0;
      if(frametable[i].p){
        frametable[i].p->resident_pages--;
      }
      break;
    }
  }
  release(&framelock);
  kfree((void*)pa);
}


// Free swap slot clear acutual data in the swap space at the given index
void free_swap_slot(int idx){
  if(idx<0 || idx>=MAX_SWAP_PAGES){
    return;
  }
  acquire(&swaplock);
  swaptable[idx].in_use=0;
  swaptable[idx].p=0;

  memset(swapspace[idx],0,PGSIZE);
  release(&swaplock);
}

// Free swap pages of a process when process exits
void free_proc_swap(struct proc *p){
  acquire(&swaplock);
  for(int i=0;i<MAX_SWAP_PAGES;i++){
    if(swaptable[i].in_use && swaptable[i].p==p){
      swaptable[i].in_use=0;
      swaptable[i].p=0;
      memset(swapspace[i],0,PGSIZE);
    }
  }
  release(&swaplock);
}