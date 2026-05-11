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

int raid_mode=0; // 0: striping, 1: mirroring, 5: parity, 10: mirroring,striping
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
        else if(mode==1){
            uint pair=logical_block%2;
            uint offset=logical_block/2;
            uint diskA=pair*2;
            uint diskB=pair*2+1;
            uint disk_blockA=SWAP_START_BLOCK+diskA*(SIM_DISK_SIZE)+offset;
            uint disk_blockB=SWAP_START_BLOCK+diskB*(SIM_DISK_SIZE)+offset;
            
            // do not write in the failed disk and we can recover data of failed disk
            // by its copy disk
            if(diskA!=failed_disk){
                struct buf* bA=bget(1,disk_blockA);
                memmove(bA->data,(void*)(pa+i*BSIZE),BSIZE);
                bwrite(bA);
                brelse(bA);
            }
            if(diskB!=failed_disk){
                struct buf* bB=bget(1,disk_blockB);
                memmove(bB->data,(void*)(pa+i*BSIZE),BSIZE);
                bwrite(bB);
                brelse(bB);
            }
        }
        else if(mode==10){
            // mirrioring
            uint pairs=logical_block%(NUM_DISKS/2);
            // striping
            uint offset=logical_block/(NUM_DISKS/2);
            uint diskA=2*pairs;
            uint diskB=2*pairs+1;
            uint disk_blockA=SWAP_START_BLOCK+diskA*(SIM_DISK_SIZE)+offset;
            uint disk_blockB=SWAP_START_BLOCK+diskB*(SIM_DISK_SIZE)+offset;
            // write to the disks which are not failed
            if(diskA!=failed_disk){
                struct buf* bA=bget(1,disk_blockA);
                memmove(bA->data,(void*)(pa+i*BSIZE),BSIZE);
                bwrite(bA);
                brelse(bA);
            }
            if(diskB!=failed_disk){
                struct buf* bB=begt(1,disk_blockB);
                memmove(bB->data,(void*)(pa+i*BSIZE),BSIZE);
                bwrite(bB);
                brelse(bB);
            }
        }
        else if(mode==5){
            uint offset=logical_block/(NUM_DISKS-1);
            uint data_disk=logical_block%(NUM_DISKS-1);
            uint parity_disk=offset%NUM_DISKS;
            if(data_disk>=parity_disk){
                data_disk++;
            }
            uint data_disk_block=SWAP_START_BLOCK+(data_disk*SIM_DISK_SIZE)+offset;
            uint parity_disk_block=SWAP_START_BLOCK+(parity_disk*SIM_DISK_SIZE)+offset;
            if(disk_failed==data_disk){
                struct buf *bparity=bread(1,parity_disk_block);
                char *new_data=(char *)(pa+i*BSIZE);
                memmove(bparity->data,new_data,BSIZE);
                for(int d=0;d<NUM_DISKS;d++){
                    if(d==disk_failed || d==parity_disk){
                        continue;
                    }

                    uint disk_block_other=SWAP_START_BLOCK+(d*SIM_DISK_SIZE)+offset;
                    struct buf *bother=bread(1,disk_block_other);
                    for(int j=0;j<BSIZE;j++){
                        bparity->data[j]^=bother->data[j];
                    }
                    brelse(bother);
                }
                bwrite(bparity);
                brelse(bparity);
            }
            else if(disk_failed==parity_disk){
                struct buf* bdata=bread(1,data_disk_block);
                memmove(bdata->data,(void*)(pa+i*BSIZE),BSIZE);
                bwrite(bdata);
                brelse(bdata);
            }
            else{
                struct buf *bdata,*bparity;
                // lock buffers in fixed block order to avoid deadlock
                if(data_disk_block<parity_disk_block){
                    bdata=bread(1,data_disk_block);
                    bparity=bread(1,parity_disk_block);
                } 
                else{
                    bparity=bread(1,parity_disk_block);
                    bdata=bread(1,data_disk_block);
                }
                // using subtractive parity method to update the parity disk
                char *new_data=(void*)(pa+i*BSIZE);
                for(int j=0;j<BSIZE;j++){
                    bparity->data[j]^=(bdata->data[j]^new_data[j]);
                }
                memmove(bdata->data,new_data,BSIZE);
                bwrite(bdata);
                bwrite(bparity);
                brelse(bdata);
                brelse(bparity);
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
        else if(mode==1){
            uint pair=logical_block%2;
            uint offset=logical_block/2;
            uint diskA=pair*2;
            uint diskB=pair*2+1;
            uint disk_blockA=SWAP_START_BLOCK+diskA*(SIM_DISK_SIZE)+offset;
            uint disk_blockB=SWAP_START_BLOCK+diskB*(SIM_DISK_SIZE)+offset;

            // read from the disk which is not failed
            uint read_disk=(disk_failed==diskA)?disk_blockB:disk_blockA;
            struct buf *b=bread(1,read_disk);
            memmove((void*)(pa+i*BSIZE),b->data,BSIZE);
            brelse(b);
        }
        else if(mode==10){
            // mirrioring
            uint pairs=logical_block%(NUM_DISKS/2);
            // striping
            uint offset=logical_block/(NUM_DISKS/2);
            uint diskA=2*pairs;
            uint diskB=2*pairs+1;
            uint disk_blockA=SWAP_START_BLOCK+diskA*(SIM_DISK_SIZE)+offset;
            uint disk_blockB=SWAP_START_BLOCK+diskB*(SIM_DISK_SIZE)+offset;
            // read from the disk which is not failed
            uint read_disk=(disk_failed==diskA)?disk_blockB:disk_blockA;
            struct buf *b=bread(1,read_disk);
            memmove((void*)(pa+i*BSIZE),b->data,BSIZE);
            brelse(b);
        }
        else if(mode==5){

        }
    }
}