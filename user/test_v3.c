// Credit: Akshat
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void test1() {
    struct vmstats st;
    int pgsz = 4096;

    printf("\n--- Test 1: Allocation and Page Faults ---\n");

    getvmstats(getpid(), &st);
    printf("Initial: faults=%d, resident=%d\n",
           st.page_faults, st.resident_pages);

    char *mem = sbrklazy(50 * pgsz);

    if(mem == (char*)-1) {
        printf("sbrk failed\n");
        return;
    }

    getvmstats(getpid(), &st);
    printf("After sbrk (Lazy): faults=%d, resident=%d\n",
           st.page_faults, st.resident_pages);

    for(int i = 0; i < 50; i++) {
        mem[i * pgsz] = 'A';
    }

    getvmstats(getpid(), &st);
    printf("After touching: faults=%d, resident=%d\n",
           st.page_faults, st.resident_pages);

    printf("Test 1 Complete.\n\n");
}

void test2() {
    struct vmstats st;
    int pgsz = 4096;
    int max_pages = 40000;

    char *base = sbrk(0);

    printf("\n--- Test 2: Eviction, Verification, and Swap-In ---\n");
    printf("Allocating memory until evictions trigger...\n");

    int pages_allocated = 0;

    for(int i = 0; i < max_pages; i++) {

        char *mem = sbrklazy(pgsz);

        if(mem == (char*)-1) {
            printf("sbrk failed at page %d\n", i);
            break;
        }

        mem[0] = (char)(i % 256);

        pages_allocated++;

        getvmstats(getpid(), &st);

        if(st.pages_evicted > 150) {
            printf("Successfully triggered %d evictions!\n",
                   st.pages_evicted);
            break;
        }
    }

    printf("Reading back memory to verify and trigger swap-ins...\n");

    int corruption_errors = 0;

    char *curr = base;

    for(int i = 0; i < pages_allocated; i++) {

        if(curr[0] != (char)(i % 256)) {
            corruption_errors++;
        }

        curr += pgsz;
    }

    getvmstats(getpid(), &st);

    printf("Verification complete. Errors: %d\n",
           corruption_errors);

    printf("Pages swapped in: %d\n",
           st.pages_swapped_in);

    if(corruption_errors == 0 &&
       st.pages_swapped_in > 0) {

        printf("Test 2 PASSED!\n\n");

    } else {

        printf("Test 2 FAILED!\n\n");
    }
}

void test3() {

    printf("\n--- Test 3: Scheduler-Aware Eviction ---\n");

    int fd_B_to_A[2];
    int fd_A_to_B[2];

    if(pipe(fd_B_to_A) < 0 ||
       pipe(fd_A_to_B) < 0) {
        return;
    }

    int pid_B = fork();

    if(pid_B == 0) {

        int pgsz = 4096;
        int pages = 8000;

        char *mem = sbrklazy(pages * pgsz);

        if(mem == (char*)-1) {
            printf("Child B allocation failed\n");
            exit(1);
        }

        for(int i = 0; i < pages; i++) {
            mem[i * pgsz] = 'B';
        }

        int mypid = getpid();

        write(fd_B_to_A[1], &mypid, sizeof(mypid));

        char buf[1];

        read(fd_A_to_B[0], buf, 1);

        exit(0);
    }

    int pid_A = fork();

    if(pid_A == 0) {

        int child_b_pid;

        read(fd_B_to_A[0],
             &child_b_pid,
             sizeof(child_b_pid));

        int pgsz = 4096;
        int initial_pages = 18000;

        char *base_mem =
            sbrklazy(initial_pages * pgsz);

        if(base_mem == (char*)-1) {
            printf("Child A initial allocation failed\n");
            exit(1);
        }

        for(int i = 0; i < initial_pages; i++) {
            base_mem[i * pgsz] = 'A';
        }

        for(volatile int i = 0;
            i < 50000000;
            i++);

        struct vmstats st_A;
        struct vmstats st_B;

        for(int i = 0; i < 5000; i++) {

            char *mem = sbrklazy(pgsz);

            if(mem == (char*)-1) {
                break;
            }

            mem[0] = 'X';

            getvmstats(getpid(), &st_A);

            getvmstats(child_b_pid, &st_B);

            if((st_A.pages_evicted +
                st_B.pages_evicted) >= 50) {
                break;
            }
        }

        getvmstats(getpid(), &st_A);

        getvmstats(child_b_pid, &st_B);

        printf("Child A (CPU Bound) Evictions: %d\n",
               st_A.pages_evicted);

        printf("Child B (IO Bound) Evictions: %d\n",
               st_B.pages_evicted);

        write(fd_A_to_B[1], "y", 1);

        exit(0);
    }

    close(fd_B_to_A[0]);
    close(fd_B_to_A[1]);

    close(fd_A_to_B[0]);
    close(fd_A_to_B[1]);

    wait(0);
    wait(0);

    printf("Test 3 Complete. (Child A should have more evictions than Child B)\n\n");
}

void test4() {

    struct vmstats st;

    printf("\n--- Test 4: Edge Cases ---\n");

    char *current = sbrk(0);

    printf("Current heap top: %p\n", current);

    if(sbrklazy(5 * 4096) == (char*)-1) {
        printf("5-page allocation failed\n");
        return;
    }

    for(int i = 0; i < 5; i++) {
        current[i * 4096] = 'Z';
    }

    getvmstats(getpid(), &st);

    int res_before = st.resident_pages;

    sbrk(-3 * 4096);

    getvmstats(getpid(), &st);

    printf("Resident before shrink: %d, after shrink: %d\n",
           res_before,
           st.resident_pages);

    char *huge = sbrk(40000 * 4096);

    if(huge == (char*)-1) {

        printf("Successfully caught Out-Of-Memory error from kernel!\n");

    } else {

        for(int i = 0; i < 40000; i++) {
            huge[i * 4096] = 'Y';
        }
    }

    printf("Test 4 Complete.\n\n");
}

int main() {
    test1();
    test2();
    test3();
    test4();
    exit(0);
}
