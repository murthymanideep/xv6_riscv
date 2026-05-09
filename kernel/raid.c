#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#include "raid.h"

int raid_mode=0; // 0: striping, 1: mirroring, 5: parity
int failed_disk=-1; // -1 means all disks are healty

int get_raid_mode(void){
    return raid_mode;
}

int get_failed_disk(void){
    return failed_disk;
}

// Write from memory page to the disk blocks 
void raid_write_page(int swap_slot,uint64 pa,int mode,int disk_failed){
    // write data from buffer(page) to disk 
    // page contains 4 disk blocks
    for(int i=0;i<4;i++){
        uint logical_block=(swap_slot*4)+i;
        // Striping
        if(mode==0){
            uint disk=logical_block%NUM_DISKS;
            if(disk!=failed_disk){
                uint offset=logical_block/NUM_DISKS;
                // disk block number
                uint disk_block=SWAP_START_BLOCK+(disk*SIM_DISK_SIZE)+offset;
                // get a buffer block to the associated disk block 
                // to make reads and writes to the disk block
                struct buf* b=bget(1,disk_block);
                // write the data into buffer
                memmove(b->data,(void*)(pa+i*BSIZE),BSIZE);
                bwrite(b);
                brelse(b);
            }
        }
    }
}

// Read from disk blocks and bring data into memory
void raid_read_page(int swap_slot,uint64 pa,int mode,int disk_failed){
    for(int i=0;i,4;i++){
        uint logical_block=(swap_slot*4)+i;
        // Striping
        if(mode==0){
            uint disk=logical_block%NUM_DISKS;
            if(disk!=failed_disk){
                uint offset=logical_block/NUM_DISKS;
                // disk block number
                uint disk_block=SWAP_START_BLOCK+disk*(SIM_DISK_SIZE)+offset;
                // get a buffer block to read from associated disk block
                struct buf* b=bread(1,disk_block);
                // read the data from buffer
                memmove((void*)(pa+i*BSIZE),b->data,BSIZE);
                brelse(b);
            }
            else{
                memset((void*)(pa + i * BSIZE), 0, BSIZE);
            }
        }
    }
}