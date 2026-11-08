#include "headtrack.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define HEADTRACK_BIN     "/usr/bin/gs-headtrack"
#define HEADTRACK_PIDFILE "/var/run/gs-headtrack.pid"   /* written by S99headtrack */

bool headtrack_available(void)
{
    return access(HEADTRACK_BIN, X_OK) == 0;
}

/* The pid from the pid file, if that process is still gs-headtrack. A stale
 * pid file can name an unrelated process by now, and SIGUSR1 would kill it. */
static pid_t headtrack_pid(void)
{
    FILE * f = fopen(HEADTRACK_PIDFILE, "r");
    if(!f) return -1;
    long pid = -1;
    if(fscanf(f, "%ld", &pid) != 1) pid = -1;
    fclose(f);
    if(pid <= 1) return -1;

    char path[64], cmdline[512];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
    f = fopen(path, "r");
    if(!f) return -1;
    size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
    fclose(f);
    /* argv is NUL-separated: python3\0/usr/bin/gs-headtrack\0... */
    for(size_t i = 0; i < n; i++)
        if(cmdline[i] == '\0') cmdline[i] = ' ';
    cmdline[n] = '\0';
    return strstr(cmdline, "gs-headtrack") ? (pid_t)pid : -1;
}

bool headtrack_recentre(void)
{
    pid_t pid = headtrack_pid();
    if(pid < 0 || kill(pid, SIGUSR1) != 0) return false;
    printf("Head tracking: re-centred (pid %d)\n", (int)pid);
    fflush(stdout);
    return true;
}
