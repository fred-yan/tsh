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
    int  emit_prompt = 1;   /* suppressed by the -p flag (used by sdriver.pl) */

    /* -p flag: suppress the prompt so trace output matches tshref exactly */
    if (argc >= 2 && strcmp(argv[1], "-p") == 0)
        emit_prompt = 0;

    /* Install signal handlers */
    Signal(SIGINT,  sigint_handler);
    Signal(SIGTSTP, sigtstp_handler);
    Signal(SIGCHLD, sigchld_handler);

    /* Initialise the job list */
    initjobs(jobs);

    /* ── REPL ── */
    while (1) {
        if (emit_prompt) {
            printf("tsh> ");
            fflush(stdout);
        }

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
 * Parses @cmdline into an argv[] array via parseline(), then:
 *
 *   1. Ignores empty lines and parse errors (argv[0] == NULL or bg == -1).
 *   2. Dispatches built-in commands (quit, jobs, bg, fg) to builtin_cmd(); returns
 *      immediately if the command was a built-in.
 *   3. For external commands: blocks SIGCHLD, forks a child, moves it into
 *      its own process group with setpgid(0,0) so that SIGINT/SIGTSTP from
 *      the terminal are not delivered to the shell itself (Requirement 11.1),
 *      then exec's the command via execve().  The child restores its signal
 *      mask before exec so the executed program inherits no blocked signals.
 *      If execve fails the child prints an error message and exits.
 *   4. In the parent, registers the new job with addjob() while SIGCHLD is
 *      still blocked — guaranteeing addjob() always precedes any
 *      sigchld_handler deletejob() call (Requirements 10.1, 10.2).
 *      If the job list is full the child is killed immediately with SIGKILL.
 *   5. SIGCHLD is unblocked (prev mask restored) after addjob() completes.
 *   6. Background jobs (&): prints the standard "[jid] (pid) cmdline"
 *      notification and returns immediately.
 *   7. Foreground jobs: blocks in waitfg() until the job exits or stops.
 *
 * sigprocmask race protection (blocking SIGCHLD around fork/addjob) is
 * included here per task 9.2 (Requirements 10.1, 10.2, 10.3, 10.4).
 *
 * Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 10.1, 10.2, 10.3, 10.4, 11.1
 * ─────────────────────────────────────────────────────────────────────────── */
void eval(char *cmdline)
{
    char     *argv[MAXARGS];
    int       bg;
    pid_t     pid;
    sigset_t  mask_chld, prev; /* for SIGCHLD blocking */

    bg = parseline(cmdline, argv);
    if (bg == -1 || argv[0] == NULL)
        return;
    if (builtin_cmd(argv))
        return;

    /* Block SIGCHLD before fork to prevent race where child exits
     * before addjob() is called (Requirements 10.1, 10.4) */
    sigemptyset(&mask_chld);
    sigaddset(&mask_chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask_chld, &prev);

    if ((pid = fork()) < 0)
        unix_error("fork error");

    if (pid == 0) {
        /* Child: restore signal mask BEFORE execve so that the
         * executed program inherits no artificially blocked signals
         * (Requirements 10.3, 10.4) */
        sigprocmask(SIG_SETMASK, &prev, NULL);
        setpgid(0, 0);
        if (execve(argv[0], argv, environ) < 0) {
            fprintf(stderr, "%s: Command not found\n", argv[0]);
            exit(1);
        }
    }

    /* Parent: addjob THEN unblock — guarantees addjob always runs
     * before any sigchld_handler deletejob (Requirements 10.1, 10.2) */
    if (!addjob(jobs, pid, bg ? BG : FG, cmdline)) {
        sigprocmask(SIG_SETMASK, &prev, NULL);
        kill(-pid, SIGKILL);
        return;
    }
    sigprocmask(SIG_SETMASK, &prev, NULL); /* unblock SIGCHLD (Req 10.2) */

    if (bg) {
        printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
    } else {
        waitfg(pid);
    }
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
 * Handles: quit (terminates the shell), jobs (lists background jobs),
 *          bg and fg (resume a stopped or background job).
 * ─────────────────────────────────────────────────────────────────────────── */
int builtin_cmd(char **argv)
{
    if (strcmp(argv[0], "quit") == 0)
        exit(0);                  /* quit: terminate the shell             */
    if (strcmp(argv[0], "jobs") == 0) {
        listjobs(jobs);           /* jobs: list all background jobs        */
        return 1;
    }
    if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0) {
        do_bgfg(argv);            /* bg/fg: resume a stopped/bg job        */
        return 1;
    }
    return 0;                     /* not a built-in command                */
}

/* ───────────────────────────────────────────────────────────────────────────
 * do_bgfg — implement the bg and fg built-in commands
 *
 * Resumes a stopped (ST) or already-running background (BG) job, identified
 * by either a PID (plain digits) or a job ID (%JID form).
 *
 * Argument validation:
 *   - If argv[1] is NULL, prints a missing-argument error and returns.
 *   - If argv[1] starts with '%', interprets the rest as a decimal JID and
 *     looks the job up via getjobjid(); prints an error if not found.
 *   - Otherwise argv[1] must be a string of pure digits (PID form); any
 *     non-digit character triggers a format error.  The job is looked up via
 *     getjobpid(); prints an error if not found.
 *
 * On success:
 *   - Sends SIGCONT to the job's entire process group (kill with negative PID)
 *     so all processes in the group are resumed (Requirements 6.3, 11.3).
 *   - "fg": transitions the job to FG state and blocks in waitfg() until it
 *     exits or stops again (Requirements 6.1, 6.5, 3.6).
 *   - "bg": transitions the job to BG state and prints the standard
 *     "[jid] (pid) cmdline" notification (Requirements 6.2, 3.7).
 *
 * Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 3.6, 3.7, 11.3
 * ─────────────────────────────────────────────────────────────────────────── */
void do_bgfg(char **argv)
{
    job_t *job;
    int    jid;
    pid_t  pid;
    char  *id_arg = argv[1];  /* the PID or %JID argument */

    /* ── Validate argument presence ───────────────────────────────────────── */
    if (id_arg == NULL) {
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }

    /* ── Resolve job by %JID or PID ───────────────────────────────────────── */
    if (id_arg[0] == '%') {
        /* JID form: must be '%' followed by one or more digits */
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
        /* PID form: must be all digits */
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

    /* ── Send SIGCONT to the job's process group ───────────────────────────── */
    kill(-(job->pid), SIGCONT);

    /* ── Update state and wait (fg) or print notification (bg) ────────────── */
    if (strcmp(argv[0], "fg") == 0) {
        job->state = FG;
        waitfg(job->pid);
    } else { /* bg */
        job->state = BG;
        printf("[%d] (%d) %s", job->jid, (int)job->pid, job->cmdline);
    }
}

/* ───────────────────────────────────────────────────────────────────────────
 * waitfg — block the shell until the foreground job exits or stops
 *
 * Busy-wait loop: sleep 1 second at a time and recheck whether the
 * foreground job is still present and still in FG state.
 * Job removal and state transitions are handled exclusively by
 * sigchld_handler — we must NOT call waitpid here.
 *
 * Requirements: 12.1, 12.2, 12.3, 12.4
 * ─────────────────────────────────────────────────────────────────────────── */
void waitfg(pid_t pid)
{
    /*
     * Busy-wait loop: sleep 1 second at a time and recheck whether the
     * foreground job is still present and still in FG state.
     * Job removal and state transitions are handled exclusively by
     * sigchld_handler — we must NOT call waitpid here.
     *
     * Requirements: 12.1, 12.2, 12.3, 12.4
     */
    while (1) {
        job_t *job = getjobpid(jobs, pid);
        if (job == NULL || job->state != FG)
            break;
        sleep(1);
    }
}

/* ───────────────────────────────────────────────────────────────────────────
 * Signal handlers
 * ─────────────────────────────────────────────────────────────────────────── */

/*
 * sigchld_handler — reap zombie children and update the job list.
 *
 * Called whenever one or more children change state (exit, killed by signal,
 * or stopped).  Loops with waitpid(WNOHANG|WUNTRACED) to drain all pending
 * state changes in a single handler invocation, because SIGCHLD is not queued
 * — only one signal is pending no matter how many children changed state.
 *
 * For each reaped child:
 *   - Normal exit        → delete the job from the job list.
 *   - Killed by signal   → print a termination message, delete the job.
 *   - Stopped by signal  → update job state to ST, print a stop message.
 *
 * errno is saved and restored to preserve async-signal safety.
 *
 * Requirements: 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 3.6, 8.4
 */
void sigchld_handler(int sig)
{
    int    olderrno = errno; /* save errno — must restore before returning    */
    int    status;
    pid_t  pid;
    job_t *job;

    (void)sig; /* parameter unused; sig is always SIGCHLD here */

    /*
     * Loop with WNOHANG|WUNTRACED:
     *   WNOHANG   — return immediately if no child has changed state
     *   WUNTRACED — also report stopped (not just exited) children
     *
     * We loop because multiple children may have changed state since the
     * last delivery; SIGCHLD is not queued — only one signal is pending
     * regardless of how many children exited, so a single waitpid call
     * would miss all but the first.
     *
     * Requirements: 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 3.6, 8.4
     */
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {

        job = getjobpid(jobs, pid);
        if (job == NULL)
            continue; /* unknown PID — silently ignore (Requirement 9.6) */

        if (WIFEXITED(status)) {
            /* Child exited normally */
            deletejob(jobs, pid);

        } else if (WIFSIGNALED(status)) {
            /* Child terminated by an uncaught signal (e.g. SIGINT) */
            printf("Job [%d] (%d) terminated by signal %d\n",
                   job->jid, pid, WTERMSIG(status));
            deletejob(jobs, pid);

        } else if (WIFSTOPPED(status)) {
            /* Child stopped (e.g. SIGTSTP) */
            job->state = ST;
            printf("Job [%d] (%d) stopped by signal %d\n",
                   job->jid, pid, WSTOPSIG(status));
        }
    }

    errno = olderrno; /* restore errno before returning (async-signal safety) */
}

/*
 * sigint_handler — forward SIGINT to the foreground process group.
 *
 * When the user types Ctrl-C, the terminal delivers SIGINT to every process
 * in the foreground process group — including tsh itself.  We intercept it
 * here and re-deliver it to the process GROUP of the current foreground job
 * using kill(-pid, SIGINT), so the entire job (including any children it may
 * have forked) receives the signal, while tsh is unaffected.
 *
 * Requirements: 7.1, 7.2, 7.3, 11.2, 11.4
 */
void sigint_handler(int sig)
{
    int   olderrno = errno;
    pid_t pid = fgpid(jobs);  /* PID of current foreground job (0 if none) */

    (void)sig;

    if (pid != 0)
        kill(-pid, SIGINT);   /* negative: send to entire process group     */

    errno = olderrno;
}

/*
 * sigtstp_handler — forward SIGTSTP to the foreground process group.
 *
 * Ctrl-Z delivers SIGTSTP to the terminal's foreground process group.
 * We forward it to the foreground job's process group with kill(-pid, SIGTSTP)
 * so the job (and its children) are stopped.  sigchld_handler will then
 * update the job state to ST when the kernel delivers SIGCHLD.
 *
 * Requirements: 8.1, 8.2, 8.3, 8.4, 11.2, 11.4
 */
void sigtstp_handler(int sig)
{
    int   olderrno = errno;
    pid_t pid = fgpid(jobs);  /* PID of current foreground job (0 if none) */

    (void)sig;

    if (pid != 0)
        kill(-pid, SIGTSTP);  /* negative: send to entire process group     */

    errno = olderrno;
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
