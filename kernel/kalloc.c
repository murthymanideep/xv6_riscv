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
#include "raid.h"

#define NUM_FRAMES 64

struct frame{
  int in_use;
  struct proc *p;
  uint64 va;
  uint64 pa;
  int ref;
};
struct frame frametable[NUM_FRAMES];
struct spinlock framelock;
int clock_hand=0;
struct swapslot{
  int in_use;
  struct proc *p;
};
struct swapslot swaptable[MAX_SWAP_PAGES];
// char swapspace[MAX_SWAP_PAGES][PGSIZE];
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


uint64 swap_out(struct proc *victim_p, pte_t *victim_pte, uint64 victim_pa) {
  int s_idx = -1;
  
  acquire(&swaplock);
  for(int i = 0; i < MAX_SWAP_PAGES; i++){
    if(!swaptable[i].in_use){
      s_idx = i;
      swaptable[s_idx].in_use = 1;
      swaptable[s_idx].p = victim_p;
      break;
    }
  }
  release(&swaplock); 
  
  if(s_idx == -1) panic("swap space full");

  int flags = PTE_FLAGS(*victim_pte);
  flags = (flags & ~PTE_V) | PTE_S;
  *victim_pte = ((uint64)s_idx << 10) | flags;
  sfence_vma();

  victim_p->resident_pages--;
  victim_p->pages_evicted++;
  victim_p->pages_swapped_out++;

  raid_write_page(s_idx, victim_pa, get_raid_mode());
  return victim_pa;
}

void swap_in(struct proc *p, uint64 va, uint64 new_pa) {
  pte_t *pte = walk(p->pagetable, va, 0);
  if(pte == 0){
    panic("swap_in_page: no pte");
  }
  int s_idx = (*pte >> 10);
  if(s_idx < 0 || s_idx >= MAX_SWAP_PAGES){
    panic("swap_in_page: invalid index");
  }

  raid_read_page(s_idx, new_pa, get_raid_mode());
  
  free_swap_slot(s_idx);

  int flags = PTE_FLAGS(*pte);
  flags = (flags & ~PTE_S) | PTE_V;
  *pte = PA2PTE(new_pa) | flags;

  p->pages_swapped_in++;

  sfence_vma();
}

// using clock algorithm find the victim
int get_victim(){
  int num_frames_scanned=0;
  int low_pri=-1;

  for(int i=0;i<NUM_FRAMES;i++){
    struct frame *f=&frametable[i];
    if(f->in_use && f->p && f->p->pid != 1 && f->p->pid != 2){ 
      if(f->p->level>low_pri){
        low_pri=f->p->level;
      }
    }
  }

  if(low_pri==-1){
    panic("get_victim: no valid frames to evict");
  }

  while(num_frames_scanned<NUM_FRAMES*2){
    int idx=clock_hand;
    clock_hand=(clock_hand+1)%NUM_FRAMES;
    struct frame *f=&frametable[idx];

    if(f->in_use==1 && f->p && f->p->pagetable){
      if(f->p->pid == 1 || f->p->pid == 2) {
        num_frames_scanned++;
        continue;
      }

      if(f->p->level!=low_pri){
        num_frames_scanned++;
        continue;
      }
      
      pte_t *pte=walk(f->p->pagetable,f->va,0);

      if(!pte || !(*pte & PTE_V)){
        num_frames_scanned++;
        continue;
      }
      if(PTE2PA(*pte)!=f->pa){
        num_frames_scanned++;
        continue;
      }

      if(*pte & PTE_A){
        f->ref = 1; 
        *pte &= ~PTE_A;
        sfence_vma();
      } 
      
      if(f->ref == 1){
        f->ref = 0;
      } else {
        return idx;
      }
    }

    num_frames_scanned++;
  }

  num_frames_scanned=0;
  while(num_frames_scanned<NUM_FRAMES*2){
    int idx=clock_hand;
    clock_hand=(clock_hand+1)%NUM_FRAMES;
    struct frame *f=&frametable[idx];

    if(f->in_use==1 && f->p && f->p->pagetable){
      if(f->p->pid==1 || f->p->pid==2){
        num_frames_scanned++;
        continue;
      }

      pte_t *pte=walk(f->p->pagetable,f->va,0);

      if(!pte || !(*pte & PTE_V)){
        num_frames_scanned++;
        continue;
      }

      if(PTE2PA(*pte)!=f->pa){
        num_frames_scanned++;
        continue;
      }

      if(*pte & PTE_A){
        f->ref=1; 
        *pte&=~PTE_A;
        sfence_vma();
      } 
      
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


void* kalloc_user(uint64 va,struct proc *p){
  acquire(&framelock);
  int slot = -1;
  uint64 pa = 0;
  
  //Try to find an empty frame
  for(int i=0;i<NUM_FRAMES;i++){
    if(!frametable[i].in_use){
      slot=i;
      break;
    }
  }

  // If empty slot found, allocate physical memory
  if(slot!=-1) {
    void *mem=kalloc();
    if(mem!=0){
      pa=(uint64)mem;
    } 
    else{
      slot=-1; 
    }
  }

  //If no empty slot, EVICTION TIME
  if(slot==-1){
    struct frame *f;
    uint64 victim_pa;
    struct proc *victim_p;
    uint64 victim_va;
    pagetable_t victim_pt;
    pte_t *pte;

    for(;;){
      slot=get_victim();
      f=&frametable[slot];
      victim_pa=f->pa;
      victim_p=f->p;
      victim_va=f->va;
      victim_pt=victim_p ? victim_p->pagetable : 0;

      if(victim_p == 0 || victim_pt == 0 || victim_pa == 0){
        continue;
      }

      pte = walk(victim_pt, victim_va, 0);
      if(pte == 0 || (*pte & PTE_V) == 0 || PTE2PA(*pte) != victim_pa){
        continue;
      }

      *pte &= ~PTE_V;
      sfence_vma();
      f->in_use = 2;
      break;
    }
    
    release(&framelock); 
    pa = swap_out(victim_p, pte, victim_pa);
    
    memset((void*)pa, 5, PGSIZE); 
    acquire(&framelock);
    f->pa = pa;
    f->p = p;
    f->va = PGROUNDDOWN(va);
    f->ref = 1;
    f->in_use = 1; // Unpin and make it active
    
    p->resident_pages++;
    release(&framelock);
    
    return (void*)pa;
  }

  //Setup standard frame if no eviction was needed
  frametable[slot].pa = pa;
  frametable[slot].va = PGROUNDDOWN(va);
  frametable[slot].p = p;
  frametable[slot].in_use = 1;
  frametable[slot].ref = 1;

  p->resident_pages++;

  release(&framelock);
  return (void*)pa;
}

void kfree_user(uint64 pa){
  acquire(&framelock);
  int is_pinned = 0;
  
  for(int i=0;i<NUM_FRAMES;i++){
    if(frametable[i].in_use == 2 && frametable[i].pa == pa) {
      is_pinned = 1; // The frame is undergoing I/O, do not touch!
      break;
    }
    if(frametable[i].in_use == 1 && frametable[i].pa == pa){
      frametable[i].in_use=0;
      if(frametable[i].p){
        frametable[i].p->resident_pages--;
      }
      frametable[i].p=0;
      frametable[i].va=0;
      frametable[i].pa=0;
      frametable[i].ref=0;
      break;
    }
  }
  release(&framelock);
  if(!is_pinned){
    kfree((void*)pa);
  }
}

// free swap slot, clear acutual data in the swap space
void free_swap_slot(int idx){
  if(idx<0 || idx>=MAX_SWAP_PAGES){
    return;
  }
  acquire(&swaplock);
  if(swaptable[idx].in_use==0){
    release(&swaplock);
    return;
  }
  swaptable[idx].in_use=0;
  swaptable[idx].p=0;
  release(&swaplock);
}

// free swap pages of a process when process exits, clear actual swap space data
// free swap pages of a process when process exits
void free_proc_swap(struct proc *p){
  acquire(&swaplock);
  for(int i = 0; i < MAX_SWAP_PAGES; i++){
    if(swaptable[i].in_use && swaptable[i].p == p){
      swaptable[i].in_use = 0;
      swaptable[i].p = 0;
    }
  }
  release(&swaplock);
}