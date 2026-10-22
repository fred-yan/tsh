/*
 * tsh.c - Tiny Shell
 *
 * A simplified Unix shell supporting foreground/background job management,
 * built-in commands (quit, jobs, bg, fg), and POSIX signal handling
 * (SIGINT / SIGTSTP / SIGCHLD).
 *
 * Target platform: Linux
 * Build:           gcc -Wall -Wextra -o tsh tsh.c
 *
 * Design reference: .kiro/specs/tiny-shell/design.md
 * Requirements:     .kiro/specs/tiny-shell/requirements.md
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* expose POSIX + GNU extensions (environ, etc.) */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

/* =========================================================================
 * Constants
 * ========================================================================= */
#define MAXJOBS  16     /* maximum number of simultaneous jobs              */
#define MAXLINE  1024   /* maximum command-line length (chars, incl. '\0')  */
#define MAXARGS  128    /* maximum number of tokens in a command line       */

/* =========================================================================
 * Job-state constants
 * ========================================================================= */
#define UNDEF  0   /* empty slot - no job assigned                          */
#define FG     1   /* running in the foreground                             */
#define BG     2   /* running in the background                             */
#define ST     3   /* stopped (SIGTSTP received)                            */

/* =========================================================================
 * Types
 * ========================================================================= */
typedef struct job_t {
    pid_t pid;              /* process ID; 0 denotes an empty slot           */
    int   jid;              /* job ID; assigned starting from 1              */
    int   state;            /* UNDEF | FG | BG | ST                          */
    char  cmdline[MAXLINE]; /* original command line (copy stored here)      */
} job_t;

/* =========================================================================
 * Global variables
 * ========================================================================= */
job_t jobs[MAXJOBS];   /* the job list (statically allocated)               */
int   nextjid = 1;     /* next JID to allocate; strictly monotone           */
extern char **environ; /* POSIX process environment                         */

/* =========================================================================
 * Forward declarations - shell core
 * ========================================================================= */
void eval(char *cmdline);
int  builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);
int  parseline(const char *cmdline, char **argv);

/* =========================================================================
 * Forward declarations - signal handlers
 * ========================================================================= */
void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

/* =========================================================================
 * Forward declarations - job manager
 * ========================================================================= */
void   initjobs(job_t *jobs);
int    addjob(job_t *jobs, pid_t pid, int state, char *cmdline);
int    deletejob(job_t *jobs, pid_t pid);
job_t *getjobpid(job_t *jobs, pid_t pid);
job_t *getjobjid(job_t *jobs, int jid);
pid_t  fgpid(job_t *jobs);
int    pid2jid(pid_t pid);
pid_t  jid2pid(int jid);
void   listjobs(job_t *jobs);

/* =========================================================================
 * Helper utilities (CSAPP-style wrappers)
 * ========================================================================= */

/* unix_error - print a perror-style message and terminate */
void unix_error(const char *msg)
{
    perror(msg);
    exit(1);
}

/* app_error - print an application-level error message and terminate */
void app_error(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

/*
 * Signal - portable sigaction wrapper (mirrors CSAPP Signal()).
 * Installs handler with SA_RESTART so slow syscalls are restarted.
 */
typedef void handler_t(int);

handler_t *Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags   = SA_RESTART;

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");

    return old_action.sa_handler;
}

/* =========================================================================
 * main
 *
 * Suppressed with -DTESTING so test binaries can link tsh.c without
 * a symbol collision.
 * ========================================================================= */
#ifndef TESTING
int main(int argc, char **argv)
{
    char cmdline[MAXLINE];
    int  emit_prompt = 1;   /* -p flag suppresses prompt (used by sdriver.pl) */

    if (argc >= 2 && strcmp(argv[1], "-p") == 0)
        emit_prompt = 0;

    Signal(SIGINT,  sigint_handler);
    Signal(SIGTSTP, sigtstp_handler);
    Signal(SIGCHLD, sigchld_handler);

    initjobs(jobs);

    while (1) {
        if (emit_prompt) {
            printf("tsh> ");
            fflush(stdout);
        }

        if (fgets(cmdline, MAXLINE, stdin) == NULL)
            exit(0);   /* EOF - exit cleanly (Requirement 1.4) */

        eval(cmdline);
    }

    return 0;
}
#endif /* TESTING */

/* =========================================================================
 * eval - evaluate a command line
 *
 * 1. Ignore empty lines and parse errors.
 * 2. Dispatch built-in commands (quit/jobs/bg/fg) via builtin_cmd().
 * 3. Fork a child; child calls setpgid(0,0) then execve().
 * 4. Block SIGCHLD before fork, call addjob() in parent, then unblock.
 *    This prevents the race where sigchld_handler runs deletejob() before
 *    addjob() has registered the job.
 * 5. Foreground jobs: block in waitfg() until done.
 *    Background jobs: print [jid] (pid) cmdline notification.
 *
 * Requirements: 2.1-2.5, 10.1-10.4, 11.1
 * ========================================================================= */
void eval(char *cmdline)
{
    char     *argv[MAXARGS];
    int       bg;
    pid_t     pid;
    sigset_t  mask_chld, prev;

    bg = parseline(cmdline, argv);
    if (bg == -1 || argv[0] == NULL)
        return;
    if (builtin_cmd(argv))
        return;

    /* Block SIGCHLD before fork (Requirements 10.1, 10.4) */
    sigemptyset(&mask_chld);
    sigaddset(&mask_chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask_chld, &prev);

    if ((pid = fork()) < 0)
        unix_error("fork error");

    if (pid == 0) {
        /* Child: restore mask before execve (Requirements 10.3, 10.4) */
        sigprocmask(SIG_SETMASK, &prev, NULL);
        setpgid(0, 0);
        if (execve(argv[0], argv, environ) < 0) {
            fprintf(stderr, "%s: Command not found\n", argv[0]);
            exit(1);
        }
    }

    /* Parent: addjob then unblock (Requirements 10.1, 10.2) */
    if (!addjob(jobs, pid, bg ? BG : FG, cmdline)) {
        sigprocmask(SIG_SETMASK, &prev, NULL);
        kill(-pid, SIGKILL);
        return;
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);

    if (bg)
        printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
    else
        waitfg(pid);
}

/* =========================================================================
 * parseline - parse cmdline into argv[], return 1 if background job.
 *
 * Returns: 1 (background), 0 (foreground), -1 (parse error).
 * Requirements: 1.2, 1.3, 1.5, 1.6, 2.2, 2.3
 * ========================================================================= */
int parseline(const char *cmdline, char **argv)
{
    static char buf[MAXLINE];
    char *tok;
    int   argc = 0;

    if (strlen(cmdline) > MAXLINE - 1) {
        fprintf(stderr, "parseline error: command line exceeds %d characters\n",
                MAXLINE - 1);
        if (argv != NULL) argv[0] = NULL;
        return -1;
    }

    strncpy(buf, cmdline, MAXLINE - 1);
    buf[MAXLINE - 1] = '\0';

    tok = strtok(buf, " \t\r\n");
    while (tok != NULL) {
        if (argc >= MAXARGS - 1) {
            fprintf(stderr, "parseline error: argument count exceeds %d\n",
                    MAXARGS - 1);
            argv[0] = NULL;
            return -1;
        }
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }

    argv[argc] = NULL;

    if (argc == 0)
        return 0;

    if (strcmp(argv[argc - 1], "&") == 0) {
        argv[argc - 1] = NULL;
        return 1;
    }

    return 0;
}

/* =========================================================================
 * builtin_cmd - handle built-in commands: quit, jobs, bg, fg.
 *
 * Returns 1 if handled, 0 if not a built-in.
 * Requirements: 4.1-4.3, 5.1-5.2, 6.1-6.2
 * ========================================================================= */
int builtin_cmd(char **argv)
{
    if (strcmp(argv[0], "quit") == 0)
        exit(0);
    if (strcmp(argv[0], "jobs") == 0) {
        listjobs(jobs);
        return 1;
    }
    if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0) {
        do_bgfg(argv);
        return 1;
    }
    return 0;
}

/* =========================================================================
 * do_bgfg - implement the bg and fg built-in commands.
 *
 * Accepts %JID or PID argument. Sends SIGCONT to the job's process group.
 * fg: sets state=FG, calls waitfg().
 * bg: sets state=BG, prints notification.
 *
 * Requirements: 6.1-6.5, 3.6-3.7, 11.3
 * ========================================================================= */
void do_bgfg(char **argv)
{
    job_t *job;
    int    jid;
    pid_t  pid;
    char  *id_arg = argv[1];

    if (id_arg == NULL) {
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }

    if (id_arg[0] == '%') {
        jid = atoi(id_arg + 1);
        if (jid <= 0) {
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return;
        }
        job = getjobjid(jobs, jid);
        if (job == NULL) {
            printf("%%%d: No such job\n", jid);
            return;
        }
    } else {
        char *p = id_arg;
        while (*p) {
            if (!isdigit((unsigned char)*p)) {
                printf("%s: argument must be a PID or %%jobid\n", argv[0]);
                return;
            }
            p++;
        }
        pid = (pid_t)atoi(id_arg);
        if (pid <= 0) {
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return;
        }
        job = getjobpid(jobs, pid);
        if (job == NULL) {
            printf("(%d): No such process\n", pid);
            return;
        }
    }

    kill(-(job->pid), SIGCONT);

    if (strcmp(argv[0], "fg") == 0) {
        job->state = FG;
        waitfg(job->pid);
    } else {
        job->state = BG;
        printf("[%d] (%d) %s", job->jid, (int)job->pid, job->cmdline);
    }
}

/* =========================================================================
 * waitfg - busy-wait until the foreground job exits or stops.
 *
 * Uses sleep(1) loop; does NOT call waitpid directly.
 * Job removal/state changes are handled exclusively by sigchld_handler.
 *
 * Requirements: 12.1-12.4
 * ========================================================================= */
void waitfg(pid_t pid)
{
    while (1) {
        job_t *job = getjobpid(jobs, pid);
        if (job == NULL || job->state != FG)
            break;
        sleep(1);
    }
}

/* =========================================================================
 * Signal handlers
 * ========================================================================= */

/*
 * sigchld_handler - reap zombie children, update job list.
 *
 * Loops with waitpid(WNOHANG|WUNTRACED) to drain all pending state
 * changes. SIGCHLD is not queued, so we must loop to catch all children
 * that may have changed state since the last handler invocation.
 *
 * Requirements: 9.1-9.6, 3.6, 8.4
 */
void sigchld_handler(int sig)
{
    int    olderrno = errno;
    int    status;
    pid_t  pid;
    job_t *job;

    (void)sig;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        job = getjobpid(jobs, pid);
        if (job == NULL)
            continue;   /* unknown PID - silently ignore (Req 9.6) */

        if (WIFEXITED(status)) {
            deletejob(jobs, pid);

        } else if (WIFSIGNALED(status)) {
            printf("Job [%d] (%d) terminated by signal %d\n",
                   job->jid, pid, WTERMSIG(status));
            deletejob(jobs, pid);

        } else if (WIFSTOPPED(status)) {
            job->state = ST;
            printf("Job [%d] (%d) stopped by signal %d\n",
                   job->jid, pid, WSTOPSIG(status));
        }
    }

    errno = olderrno;
}

/*
 * sigint_handler - forward SIGINT to the foreground process group.
 *
 * Uses kill(-pid, SIGINT) to send to the entire process group, so
 * all children of the foreground job receive the signal.
 *
 * Requirements: 7.1-7.3, 11.2, 11.4
 */
void sigint_handler(int sig)
{
    int   olderrno = errno;
    pid_t pid = fgpid(jobs);

    (void)sig;

    if (pid != 0)
        kill(-pid, SIGINT);

    errno = olderrno;
}

/*
 * sigtstp_handler - forward SIGTSTP to the foreground process group.
 *
 * Uses kill(-pid, SIGTSTP). sigchld_handler will update job state to ST
 * when the kernel delivers SIGCHLD after the job stops.
 *
 * Requirements: 8.1-8.4, 11.2, 11.4
 */
void sigtstp_handler(int sig)
{
    int   olderrno = errno;
    pid_t pid = fgpid(jobs);

    (void)sig;

    if (pid != 0)
        kill(-pid, SIGTSTP);

    errno = olderrno;
}

/* =========================================================================
 * Job manager - implementation
 * ========================================================================= */

/* initjobs - zero-initialise the job array. */
void initjobs(job_t *jobs)
{
    memset(jobs, 0, MAXJOBS * sizeof(job_t));
}

/*
 * addjob - add a new job to the job list.
 * Returns the new JID on success, 0 if the list is full.
 */
int addjob(job_t *jobs, pid_t pid, int state, char *cmdline)
{
    int i;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid   = pid;
            jobs[i].state = state;
            jobs[i].jid   = nextjid++;
            strncpy(jobs[i].cmdline, cmdline, MAXLINE - 1);
            jobs[i].cmdline[MAXLINE - 1] = '\0';
            return jobs[i].jid;
        }
    }

    fprintf(stderr, "Tried to create too many jobs\n");
    return 0;
}

/*
 * deletejob - remove the job with the given pid.
 * Returns 1 on success, 0 if not found.
 */
int deletejob(job_t *jobs, pid_t pid)
{
    int i;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            memset(&jobs[i], 0, sizeof(job_t));
            return 1;
        }
    }

    return 0;
}

/* getjobpid - return pointer to job with given pid, or NULL. */
job_t *getjobpid(job_t *jobs, pid_t pid)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

/* getjobjid - return pointer to job with given jid, or NULL. */
job_t *getjobjid(job_t *jobs, int jid)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

/* fgpid - return PID of the foreground job, or 0 if none. */
pid_t fgpid(job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return (pid_t)0;
}

/* pid2jid - return JID for the given pid (uses global jobs), or 0. */
int pid2jid(pid_t pid)
{
    job_t *job = getjobpid(jobs, pid);
    return job ? job->jid : 0;
}

/* jid2pid - return PID for the given jid (uses global jobs), or 0. */
pid_t jid2pid(int jid)
{
    job_t *job = getjobjid(jobs, jid);
    return job ? job->pid : (pid_t)0;
}

/*
 * listjobs - print all active jobs to stdout.
 * Format: [JID] (PID) Running/Stopped    cmdline
 */
void listjobs(job_t *jobs)
{
    int i;
    const char *state_str;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0)
            continue;
        state_str = (jobs[i].state == ST) ? "Stopped" : "Running";
        printf("[%d] (%d) %s    %s\n",
               jobs[i].jid, (int)jobs[i].pid, state_str, jobs[i].cmdline);
    }
}