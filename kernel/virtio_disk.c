//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"
#include "proc.h"

int disk_sched_policy=0;   // 0=FCFS, 1=SSTF
uint disk_head_position=0; // Current head position

struct buf *disk_queue=0;
struct buf *disk_active=0;
#define LATENCY_C 5

uint calculate_disk_latency(uint current_block,uint requested_block){
    uint distance=(current_block>requested_block)?(current_block-requested_block):(requested_block-current_block);
    return distance+LATENCY_C;
}

// the address of virtio mmio register r.
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))

static struct disk {
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;

  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].

  struct {
    struct buf *b;
    char status;
  } info[NUM];

  struct virtio_blk_req ops[NUM];
  struct spinlock vdisk_lock;
} disk;

void
virtio_disk_init(void)
{
  uint32 status = 0;

  initlock(&disk.vdisk_lock, "virtio_disk");
  disk_queue=0;
  disk_active=0;

  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION) != 2 ||
     *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk");
  }
  
  // reset device
  *R(VIRTIO_MMIO_STATUS) = status;

  // set ACKNOWLEDGE status bit
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  // set DRIVER status bit
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // re-read status to ensure FEATURES_OK is set.
  status = *R(VIRTIO_MMIO_STATUS);
  if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  // initialize queue 0.
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

  // ensure queue 0 is not in use.
  if(*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk should not be ready");

  // check maximum queue size.
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk has no queue 0");
  if(max < NUM)
    panic("virtio disk max queue too short");

  // allocate and zero queue memory.
  disk.desc = kalloc();
  disk.avail = kalloc();
  disk.used = kalloc();
  if(!disk.desc || !disk.avail || !disk.used)
    panic("virtio disk kalloc");
  memset(disk.desc, 0, PGSIZE);
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used, 0, PGSIZE);

  // set queue size.
  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

  // write physical addresses.
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

  // queue is ready.
  *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

  // all NUM descriptors start out unused.
  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;
}

static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

static void
free_desc(int i)
{
  if(i >= NUM)
    panic("free_desc 1");
  if(disk.free[i])
    panic("free_desc 2");
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  wakeup(&disk.free[0]);
}

static void
free_chain(int i)
{
  while(1){
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

static int
alloc3_desc(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

void
virtio_disk_rw(struct buf *b, int write)
{
  uint64 sector = b->blockno * (BSIZE / 512);
  b->p = myproc();
  
  acquire(&disk.vdisk_lock);
  
  // Enqueue request
  b->qnext = 0;
  if (disk_queue == 0) {
    disk_queue = b;
  } else {
    struct buf *p = disk_queue;
    while(p->qnext != 0) p = p->qnext;
    p->qnext = b;
  }

  // Scheduler Loop
  while(disk_active != b) {
    if (disk_active == 0 && disk_queue != 0) {
        
        struct buf *curr;
        struct buf *chosen = disk_queue;
        int best_level = 0x7fffffff;
        int found_level = 0;
        uint min_dist = 0xFFFFFFFF;

        for (struct buf *iter = disk_queue; iter != 0; iter = iter->qnext) {
            if (iter->p && iter->p->level < best_level) {
                best_level = iter->p->level;
                found_level = 1;
            }
        }

        for (curr = disk_queue; curr != 0; curr = curr->qnext) {
            if (!found_level || (curr->p && curr->p->level == best_level)) {
                if (disk_sched_policy == 0) { 
                    chosen = curr; 
                    break; 
                } 
                else {
                    uint dist=(curr->blockno>disk_head_position)?(curr->blockno-disk_head_position):(disk_head_position-curr->blockno);
                                
                    if (dist < min_dist) {
                        min_dist = dist;
                        chosen = curr;
                    }
                }
            }
        }

        disk_active = chosen;
        
        if (chosen == b) {
            break;
        } else {
            wakeup(chosen); 
        }
    }
    sleep(b, &disk.vdisk_lock);
  }

  // Compute latency BEFORE moving the disk head position
  uint local_latency = calculate_disk_latency(disk_head_position, b->blockno);
  disk_head_position = b->blockno; 
  struct proc *p = myproc();
  if (p != 0) {
      if (write) {
          p->disk_writes++;
      } else {
          p->disk_reads++;
      }
      p->total_disk_ios++;
      p->total_disk_latency += local_latency;
      
      // Calculate and store the average safely
      if (p->total_disk_ios > 0) {
          p->avg_disk_latency = p->total_disk_latency / p->total_disk_ios;
      }
  }

  int idx[3];
  while(1){
    if(alloc3_desc(idx) == 0) break;
    sleep(&disk.free[0], &disk.vdisk_lock);
  }

  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];
  buf0->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  buf0->reserved = 0;
  buf0->sector = sector;

  disk.desc[idx[0]].addr = (uint64) buf0;
  disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  disk.desc[idx[1]].addr = (uint64) b->data;
  disk.desc[idx[1]].len = BSIZE;
  disk.desc[idx[1]].flags = write ? 0 : VRING_DESC_F_WRITE;
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  disk.info[idx[0]].status = 0xff;
  disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE;
  disk.desc[idx[2]].next = 0;

  b->disk = 1;
  disk.info[idx[0]].b = b;

  disk.avail->ring[disk.avail->idx % NUM] = idx[0];
  __sync_synchronize();
  disk.avail->idx += 1;
  __sync_synchronize();

  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

  while(b->disk == 1) {
    sleep(b, &disk.vdisk_lock);
  }

  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);

  // Dequeue
  if (disk_queue == b) {
      disk_queue = b->qnext;
  } else {
      struct buf *q = disk_queue;
      while(q && q->qnext != b) q = q->qnext;
      if (q) q->qnext = b->qnext;
  }
  
  disk_active = 0; 

  // Wakeup
  struct buf *w = disk_queue;
  while(w) {
      wakeup(w);
      w = w->qnext;
  }

  release(&disk.vdisk_lock);
}

void
virtio_disk_intr()
{
  acquire(&disk.vdisk_lock);

  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  __sync_synchronize();

  while(disk.used_idx != disk.used->idx){
    __sync_synchronize();
    int id = disk.used->ring[disk.used_idx % NUM].id;

    if(disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    struct buf *b = disk.info[id].b;
    b->disk = 0;   // disk is done with buf
    wakeup(b);

    disk.used_idx += 1;
  }

  release(&disk.vdisk_lock);
}