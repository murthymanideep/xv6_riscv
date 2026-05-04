#include "kernel/types.h"
#include "user/user.h"

struct mlfqinfo info;

void cpu_bound()
{
    for(int i = 0; i < 250000000; i++);
    printf("[CPU] PID %d Level: %d\n", getpid(), getlevel());
}

void syscall_heavy()
{
    for(int i = 0; i < 2000; i++){
        getpid();
    }
    printf("[SYSCALL] PID %d Level: %d\n", getpid(), getlevel());
}

void mixed()
{
    for(int i = 0; i < 500; i++){
        for(int j = 0; j < 10000; j++);
        getpid();
    }
    printf("[MIXED] PID %d Level: %d\n", getpid(), getlevel());
}

// ---------- Test 1 ----------
void test1()
{
    printf("\n=== Test 1: Core Scheduling Behavior ===\n");

    printf("Initial Level (parent): %d\n", getlevel());

    if(fork() == 0){
        cpu_bound();
        exit(0);
    }

    if(fork() == 0){
        syscall_heavy();
        exit(0);
    }

    if(fork() == 0){
        mixed();
        exit(0);
    }

    for(int i = 0; i < 3; i++)
        wait(0);
}

// ---------- Test 2 ----------
void test2()
{
    printf("\n=== Test 2: Priority Boost & Fairness ===\n");

    for(int i = 0; i < 3; i++){
        if(fork() == 0){
            for(int j = 0; j < 300000000; j++);

            int lvl_before = getlevel();
            printf("Child %d level BEFORE boost: %d\n", getpid(), lvl_before);

            for(int k = 0; k < 200; k++){
                pause(1);
            }

            int lvl_after = getlevel();
            printf("Child %d level AFTER boost: %d\n", getpid(), lvl_after);

            if(lvl_after != 0){
                printf("ERROR: Boost failed for PID %d\n", getpid());
            }

            exit(0);
        }
    }

    for(int i = 0; i < 3; i++)
        wait(0);
}

// ---------- Test 3 ----------
void test3()
{
    printf("\n=== Test 3: getmlfqinfo + Edge Cases ===\n");

    int pid = getpid();

    for(int i = 0; i < 1000; i++){
        getpid();
        for(int j = 0; j < 10000; j++);
    }

    if(getmlfqinfo(pid, &info) < 0){
        printf("ERROR: getmlfqinfo failed\n");
        exit(1);
    }

    printf("Level: %d\n", info.level);
    printf("Times scheduled: %d\n", info.times_scheduled);
    printf("Total syscalls: %d\n", info.total_syscalls);

    for(int i = 0; i < 4; i++){
        printf("Ticks[%d]: %d\n", i, info.ticks[i]);
    }

    if(info.total_syscalls == 0){
        printf("ERROR: syscall count not tracked\n");
    }

    if(getmlfqinfo(99999, &info) != -1){
        printf("ERROR: invalid PID not handled\n");
    } else {
        printf("Invalid PID handled correctly\n");
    }
}

// ---------- Main ----------
int main()
{
    test1();
    test2();
    test3();
    exit(0);
}