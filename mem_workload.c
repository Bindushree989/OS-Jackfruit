#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int max_mib = argc > 1 ? atoi(argv[1]) : 100;
    printf("[mem_workload] allocating up to %d MiB\n", max_mib);
    fflush(stdout);
    int allocated = 0;
    while (allocated < max_mib) {
        int chunk = 5;
        if (allocated + chunk > max_mib) chunk = max_mib - allocated;
        void *p = malloc((size_t)chunk * 1024 * 1024);
        if (!p) { perror("malloc"); break; }
        memset(p, 0xAB, (size_t)chunk * 1024 * 1024);
        allocated += chunk;
        printf("[mem_workload] allocated %d MiB\n", allocated);
        fflush(stdout);
        sleep(5);
    }
    printf("[mem_workload] holding... sleeping 120s\n");
    fflush(stdout);
    sleep(120);
    return 0;
}
