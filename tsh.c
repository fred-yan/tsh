/*
 * tsh.c — Tiny Shell
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

#define _GNU_SOURCE   /* expose POSIX + GNU extensions (environ, etc.) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ───────────────────────────────────────────────────────────────────────────
 * Constants
 * ─────────────────────────────────────────────────────────────────────────── */
#define MAXJOBS  16     /* maximum number of simultaneous jobs              */
#define MAXLINE  1024   /* maximum command-line length (chars, incl. '\0')  */
#define MAXARGS  128    /* maximum number of tokens in a command line       */

/* ───────────────────────────────────────────────────────────────────────────
 * Job-state constants
 * ─────────────────────────────────────────────────────────────────────────── */
#define UNDEF  0   /* empty slot – no job assigned                          */
#define FG     1   /* running in the foreground                             */
#define BG     2   /* running in the background                             */
#define ST     3   /* stopped (SIGTSTP received)                            */

/* ───────────────────────────────────────────────────────────────────────────
 * Types
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct job_t {
    pid_t pid;              /* process ID; 0 denotes an empty slot           */
    int   jid;              /* job ID; assigned starting from 1              */
    int   state;            /* UNDEF | FG | BG | ST                          */
    char  cmdline[MAXLINE]; /* original command line (copy stored here)      */
} job_t;

/* ───────────────────────────────────────────────────────────────────────────
 * Global variables
 * ─────────────────────────────────────────────────────────────────────────── */
job_t jobs[MAXJOBS];   /* the job list (statically allocated)               */
int   nextjid = 1;     /* next JID to allocate; strictly monotone           */
extern char **environ; /* POSIX process environment                         */

/* ───────────────────────────────────────────────────────────────────────────
 * Forward declarations — shell core
 * ─────────────────────────────────────────────────────────────────────────── */
void eval(char *cmdline);
int  builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);
int  parseline(const char *cmdline, char **argv);

/* ───────────────────────────────────────────────────────────────────────────
 * Forward declarations — signal handlers
 * ─────────────────────────────────────────────────────────────────────────── */
void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

/* ───────────────────────────────────────────────────────────────────────────
 * Forward declarations — job manager
 * ─────────────────────────────────────────────────────────────────────────── */
void   initjobs(job_t *jobs);
int    addjob(job_t *jobs, pid_t pid, int state, char *cmdline);
int    deletejob(job_t *jobs, pid_t pid);
job_t *getjobpid(job_t *jobs, pid_t pid);
job_t *getjobjid(job_t *jobs, int jid);
pid_t  fgpid(job_t *jobs);
int    pid2jid(pid_t pid);
pid_t  jid2pid(int jid);
void   listjobs(job_t *jobs);

/* ───────────────────────────────────────────────────────────────────────────
 * Helper utilities (CSAPP-style wrappers)
 * ─────────────────────────────────────────────────────────────────────────── */

/* unix_error — print a perror-style message and terminate */
void unix_error(const char *msg)
{
    perror(msg);
    exit(1);
}

/* app_error — print an application-level error message and terminate */
void app_error(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

/*
 * Signal — portable sigaction wrapper (mirrors CSAPP Signal()).
 *
 * Installs @handler for signal @signum with SA_RESTART so that
 * slow system calls are automatically restarted.  Returns the
 * previous handler.
 */
typedef void handler_t(int);

handler_t *Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);       /* block nothing extra during handler */
    action.sa_flags   = SA_RESTART;     /* restart interrupted syscalls       */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");

    return old_action.sa_handler;
}

/* ───────────────────────────────────────────────────────────────────────────
 * main
 *
 * Suppressed when compiled with -DTESTING so that unit/property test
 * binaries can link tsh.c without a symbol collision.
 * ─────────────────────────────────────────────────────────────────────────── */
#ifndef TESTING
int main(int argc, char **argv)
{
    char cmdline[MAXLINE];

    /* Suppress unused-parameter warnings for flags not yet wired up */
    (void)argc;
    (void)argv;

    /* Install signal handlers */
    Signal(SIGINT,  sigint_handler);
    Signal(SIGTSTP, sigtstp_handler);
    Signal(SIGCHLD, sigchld_handler);

    /* Initialise the job list */
    initjobs(jobs);

    /* ── REPL ── */
    while (1) {
        printf("tsh> ");
        fflush(stdout);

        if (fgets(cmdline, MAXLINE, stdin) == NULL) {
            /* EOF on stdin — exit cleanly (Requirement 1.4) */
            exit(0);
        }

        eval(cmdline);
    }

    return 0; /* unreachable */
}
#endif /* TESTING */

/* ───────────────────────────────────────────────────────────────────────────
 * eval — evaluate a command line
 *
 * Stub: empty body.  Full implementation in task 9 (eval + race protection).
 * ─────────────────────────────────────────────────────────────────────────── */
void eval(char *cmdline)
{
    (void)cmdline;
}

/* ───────────────────────────────────────────────────────────────────────────
 * parseline — parse a command line into an argv[] array
 *
 * Copies cmdline into a static local buffer, then splits on whitespace
 * (" \t\r\n") using strtok so that argv[] entries point into that buffer.
 *
 * Returns:
 *   1   the job should run in the background (cmdline ended with "&")
 *   0   foreground (no trailing "&"), or empty line
 *  -1   parse error: cmdline longer than MAXLINE-1 chars, or more than
 *       MAXARGS-1 tokens; an error message is written to stderr in both cases
 *
 * Requirements: 1.2, 1.3, 1.5, 1.6, 2.2, 2.3
 * ─────────────────────────────────────────────────────────────────────────── */
int parseline(const char *cmdline, char **argv)
{
    static char buf[MAXLINE]; /* static: argv[] entries remain valid after return */
    char *tok;
    int   argc = 0;

    /* ── guard: line too long ──────────────────────────────────────────────── */
    if (strlen(cmdline) > MAXLINE - 1) {
        fprintf(stderr,
                "parseline error: command line exceeds %d characters\n",
                MAXLINE - 1);
        if (argv != NULL)
            argv[0] = NULL;
        return -1;
    }

    /* Safe copy — buf is MAXLINE bytes, cmdline fits because of check above. */
    strncpy(buf, cmdline, MAXLINE - 1);
    buf[MAXLINE - 1] = '\0';

    /* ── tokenise on whitespace ─────────────────────────────────────────────── */
    tok = strtok(buf, " \t\r\n");
    while (tok != NULL) {
        /* guard: too many tokens (need one slot for the NULL terminator) */
        if (argc >= MAXARGS - 1) {
            fprintf(stderr,
                    "parseline error: argument count exceeds %d\n",
                    MAXARGS - 1);
            argv[0] = NULL;
            return -1;
        }
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }

    argv[argc] = NULL; /* terminate the argument list */

    /* ── empty line ─────────────────────────────────────────────────────────── */
    if (argc == 0)
        return 0;

    /* ── background job? ────────────────────────────────────────────────────── */
    if (strcmp(argv[argc - 1], "&") == 0) {
        argv[argc - 1] = NULL; /* strip the "&" token */
        return 1;              /* background */
    }

    return 0; /* foreground */
}

/* ───────────────────────────────────────────────────────────────────────────
 * builtin_cmd — handle built-in commands (quit / jobs / bg / fg)
 *
 * Returns 1 if @argv[0] is a built-in command (already handled),
 *         0 if not a built-in (caller should fork/exec).
 *
 * Stub: returns 0.  Full implementation in tasks 7 and 12.
 * ─────────────────────────────────────────────────────────────────────────── */
int builtin_cmd(char **argv)
{
    (void)argv;
    return 0;
}

/* ───────────────────────────────────────────────────────────────────────────
 * do_bgfg — implement the bg and fg built-in commands
 *
 * Stub: empty body.  Full implementation in task 12.
 * ─────────────────────────────────────────────────────────────────────────── */
void do_bgfg(char **argv)
{
    (void)argv;
}

/* ───────────────────────────────────────────────────────────────────────────
 * waitfg — block the shell until the foreground job exits or stops
 *
 * Stub: empty body.  Full implementation in task 8.
 * ─────────────────────────────────────────────────────────────────────────── */
void waitfg(pid_t pid)
{
    (void)pid;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Signal handlers
 * ─────────────────────────────────────────────────────────────────────────── */

/*
 * sigchld_handler — reap zombie children and update the job list.
 * Stub: empty body.  Full implementation in task 10.
 */
void sigchld_handler(int sig)
{
    (void)sig;
}

/*
 * sigint_handler — forward SIGINT to the foreground process group.
 * Stub: empty body.  Full implementation in task 11.
 */
void sigint_handler(int sig)
{
    (void)sig;
}

/*
 * sigtstp_handler — forward SIGTSTP to the foreground process group.
 * Stub: empty body.  Full implementation in task 11.
 */
void sigtstp_handler(int sig)
{
    (void)sig;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Job manager — implementation
 * ─────────────────────────────────────────────────────────────────────────── */

/*
 * initjobs — zero-initialise the job array.
 * (Already a complete, minimal implementation used from the start.)
 */
void initjobs(job_t *jobs)
{
    memset(jobs, 0, MAXJOBS * sizeof(job_t));
}

/*
 * addjob — add a new job entry to the job list.
 *
 * Scans the array for the first slot where pid == 0 (empty).
 * Writes pid, state, jid (nextjid++), and a safe copy of cmdline.
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
 * deletejob — remove the job with the given @pid from the job list.
 *
 * Finds the slot by pid, zeroes the entire entry with memset.
 * Returns 1 on success, 0 if no matching entry was found.
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

/*
 * getjobpid — return a pointer to the job whose pid == @pid, or NULL.
 */
job_t *getjobpid(job_t *jobs, pid_t pid)
{
    int i;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid)
            return &jobs[i];
    }
    return NULL;
}

/*
 * getjobjid — return a pointer to the job whose jid == @jid, or NULL.
 */
job_t *getjobjid(job_t *jobs, int jid)
{
    int i;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].jid == jid)
            return &jobs[i];
    }
    return NULL;
}

/*
 * fgpid — return the PID of the current foreground job, or 0 if none.
 */
pid_t fgpid(job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].state == FG)
            return jobs[i].pid;
    }
    return (pid_t)0;
}

/*
 * pid2jid — return the JID for the given @pid, or 0 if not found.
 * Uses the global jobs array.
 */
int pid2jid(pid_t pid)
{
    job_t *job = getjobpid(jobs, pid);

    if (job != NULL)
        return job->jid;
    return 0;
}

/*
 * jid2pid — return the PID for the given @jid, or 0 if not found.
 * Uses the global jobs array.
 */
pid_t jid2pid(int jid)
{
    job_t *job = getjobjid(jobs, jid);

    if (job != NULL)
        return job->pid;
    return (pid_t)0;
}

/*
 * listjobs — print all non-UNDEF jobs to stdout.
 *
 * Iterates every slot where pid != 0; prints one line per job in the format:
 *   [JID] (PID) Running    cmdline
 *   [JID] (PID) Stopped    cmdline
 * State is "Running" for FG/BG jobs and "Stopped" for ST jobs.
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
