#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/types.h>

#define SIG_DFL ((void(*)(int))0)
#define SIG_IGN ((void(*)(int))1)

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGTERM 15

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

typedef unsigned int sigset_t;

struct sigaction {
    void       (*sa_handler)(int);
    sigset_t     sa_mask;
    unsigned int sa_flags;
};

int sigaction  (int sig, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigsuspend (sigset_t mask);
int kill       (pid_t pid, int sig);
#endif /* _SIGNAL_H */
