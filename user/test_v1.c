#include "kernel/types.h"
#include "user/user.h"

// ---- BASIC TEST ----
void basic_test()
{
    printf("---- BASIC TEST ----\n");

    hello();

    int pid1 = getpid();
    int pid2 = getpid2();

    if(pid1 == pid2)
        printf("getpid2() correct\n");
    else
        printf("getpid2() WRONG\n");

    int ppid = getppid();
    printf("PID: %d  PPID: %d\n", pid1, ppid);
}

// ---- CHILD TEST ----
void child_test()
{
    printf("---- CHILD TEST ----\n");

    int pid1 = fork();
    if(pid1 == 0){
        pause(10);
        exit(0);
    }

    int pid2 = fork();
    if(pid2 == 0){
        pause(20);
        exit(0);
    }

    pause(5);

    int n = getnumchild();
    printf("Number of children (expected 2): %d\n", n);

    int sc = getchildsyscount(pid1);
    printf("Child syscall count (valid child): %d\n", sc);

    int invalid = getchildsyscount(9999);
    printf("Invalid child syscall (expected -1): %d\n", invalid);

    wait(0);
    wait(0);

    int n2 = getnumchild();
    printf("Number of children after wait (expected 0): %d\n", n2);
}

// ---- FORK SYSCALL TEST ----
void fork_syscall_test()
{
    printf("---- FORK SYSCALL TEST ----\n");

    int pid = fork();

    if(pid == 0){
        int c = getsyscount();
        printf("Child syscall count: %d\n", c);
        exit(0);
    }
    else{
        wait(0);
        int p = getsyscount();
        printf("Parent syscall count: %d\n", p);
    }
}

// ---- SYSCALL COUNT TEST ----
void syscall_count_test()
{
    printf("---- SYSCALL COUNT TEST ----\n");

    int before = getsyscount();
    getpid();
    getpid();
    getpid();
    getpid();

    int after = getsyscount();

    if(after == before + 5)
        printf("Syscall counter working\n");
    else
        printf("Syscall counter WRONG\n");
}

int
main()
{
    basic_test();
    child_test();
    fork_syscall_test();
    syscall_count_test();

    exit(0);
}