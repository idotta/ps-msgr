/* runstat N CMD...: runs CMD N times from a small forked process, so the
 * RSS high-water mark is CMD's; prints CMD's output from the first run, then
 * the median and max wall time and the max RSS from wait4(). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    int n = atoi(argv[1]);
    double *w = calloc((size_t)n, sizeof *w);
    long rss = 0;
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        pid_t pid = fork();
        if (pid == 0) {
            if (i > 0) {
                int fd = open("/dev/null", O_WRONLY);
                dup2(fd, 1);
                dup2(fd, 2);
            }
            execvp(argv[2], argv + 2);
            _exit(127);
        }
        int st;
        struct rusage ru;
        wait4(pid, &st, 0, &ru);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        w[i] = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (ru.ru_maxrss > rss)
            rss = ru.ru_maxrss;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            ++bad;
    }
    qsort(w, (size_t)n, sizeof *w, cmp);
    printf("# runs %d, failed %d, wall median %.1f ms, max %.1f ms, max RSS %ld KiB\n", n, bad, w[n / 2], w[n - 1], rss);
    return bad != 0;
}
