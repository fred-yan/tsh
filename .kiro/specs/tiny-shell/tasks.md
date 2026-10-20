# Implementation Plan: Tiny Shell (tsh)

## Overview

在单一 C 源文件 `tsh.c` 中实现 Tiny Shell。按功能模块逐步构建：先搭建编译骨架与核心数据结构，再依次实现命令解析、作业管理、内置命令、前台等待、竞态防护、信号处理与 bg/fg 切换，最后通过 theft 属性测试和 sdriver.pl 16 条 trace 文件完成集成验收。每个关键模块在实现后立即附上对应的属性测试子任务，以尽早捕获正确性问题。

## Tasks

- [x] 1. Set up project skeleton and build system
  - Create `tsh.c` with all required function stubs: signatures, empty bodies, and valid return values so the file compiles cleanly
  - Create `Makefile` with targets: `tsh` (compile with `-Wall -Wextra`), `test-unit`, `test-property`, `clean`
  - Create `tests/unit/`, `tests/property/`, `tests/integration/` directory structure
  - Vendor or install the [theft](https://github.com/silentbicycle/theft) C property-based testing library under `tests/`
  - Verify that `make tsh` compiles without errors or warnings
  - _Requirements: 1.1, 1.5_

- [x] 2. Implement core types and data structures
  - [x] 2.1 Define all constants, state macros, `job_t` struct, and global variables in `tsh.c`
    - Define macros: `MAXJOBS 16`, `MAXLINE 1024`, `MAXARGS 128`
    - Define job state macros: `UNDEF 0`, `FG 1`, `BG 2`, `ST 3`
    - Define `job_t` struct: `pid_t pid`, `int jid`, `int state`, `char cmdline[MAXLINE]`
    - Declare global `job_t jobs[MAXJOBS]` and `int nextjid = 1`
    - _Requirements: 3.1, 3.2_

- [x] 3. Implement job manager functions
  - [x] 3.1 Implement `initjobs`, `addjob`, and `deletejob` in `tsh.c`
    - `initjobs(jobs)`: `memset` the entire array to zero
    - `addjob(jobs, pid, state, cmdline)`: find first slot where `pid == 0`; write `pid`, `state`, `strncpy` cmdline; assign `jid = nextjid++`; return JID on success, 0 if all slots are occupied and print an error to stderr
    - `deletejob(jobs, pid)`: find slot by pid, zero it out; return 1 on success, 0 if not found
    - _Requirements: 3.1, 3.2, 3.3_

  - [x] 3.2 Implement `getjobpid`, `getjobjid`, `fgpid`, `pid2jid`, `jid2pid`, and `listjobs` in `tsh.c`
    - `getjobpid(jobs, pid)`: linear scan; return pointer to matching entry or NULL
    - `getjobjid(jobs, jid)`: linear scan; return pointer to matching entry or NULL
    - `fgpid(jobs)`: scan for entry with `state == FG`; return its pid or 0
    - `pid2jid(pid)`: call `getjobpid`; return `job->jid` or 0
    - `jid2pid(jid)`: call `getjobjid`; return `job->pid` or 0
    - `listjobs(jobs)`: iterate non-UNDEF entries; print `[%d] (%d) %s %s\n` with JID, PID, state string (`Running`/`Stopped`), and cmdline
    - _Requirements: 3.4, 5.1, 5.2_

  - [ ]* 3.3 Write property test for `addjob` field storage round trip (Property 5)
    - Test file: `tests/property/prop_joblist.c`
    - **Property 5: addjob field storage round trip**
    - Generate arbitrary valid `(pid, state, cmdline)` triples; call `addjob`; immediately read back via `getjobpid`; assert all three fields match the inputs exactly
    - Tag: `Feature: tiny-shell, Property 5: addjob field storage round trip`
    - **Validates: Requirements 3.2**

  - [ ]* 3.4 Write property test for JID uniqueness and monotonic increment (Property 4)
    - Test file: `tests/property/prop_joblist.c`
    - **Property 4: JID uniqueness and monotonic increment**
    - Generate sequences of `addjob` and `deletejob` calls; assert all active JIDs are distinct, `nextjid` only increases, and no previously deleted JID appears in a new entry
    - Tag: `Feature: tiny-shell, Property 4: JID uniqueness and monotonicity`
    - **Validates: Requirements 3.1**

  - [ ]* 3.5 Write property test for dual-format lookup consistency (Property 6)
    - Test file: `tests/property/prop_joblist.c`
    - **Property 6: dual-format job lookup consistency**
    - For any job present in the list, assert `getjobpid(pid)` and `getjobjid(jid)` return a pointer to the same `job_t` entry (address equality)
    - Tag: `Feature: tiny-shell, Property 6: dual format lookup consistency`
    - **Validates: Requirements 3.4**

  - [ ]* 3.6 Write property test for `listjobs` completeness and format (Property 7)
    - Test file: `tests/property/prop_joblist.c`
    - **Property 7: listjobs completeness and format correctness**
    - Capture `listjobs` output via `flockfile`/pipe redirect; assert the line count equals the number of non-UNDEF entries and each line contains the correct `[JID]`, `(PID)`, state string, and cmdline
    - Tag: `Feature: tiny-shell, Property 7: listjobs completeness and format`
    - **Validates: Requirements 5.1, 5.2**

- [ ] 4. Implement `parseline`
  - [ ] 4.1 Implement `parseline` in `tsh.c`
    - Tokenise cmdline by spaces and tabs; populate `argv[]` with pointers into a local copy of the string; set `argv[n] = NULL` as terminator
    - If the last token equals `"&"`, remove it and return `1` (background); otherwise return `0` (foreground)
    - If `strlen(cmdline) > MAXLINE` or token count exceeds `MAXARGS`, print an error message to stderr describing which limit was exceeded and return `-1`
    - _Requirements: 1.2, 1.3, 1.5, 1.6, 2.2, 2.3_

  - [ ]* 4.2 Write property test for parseline comprehensive behavior (Property 1)
    - Test file: `tests/property/prop_parseline.c`
    - **Property 1: parseline comprehensive behavior**
    - Generate arbitrary valid cmdlines (printable ASCII, length ≤ MAXLINE, token count ≤ MAXARGS); assert `argv[0]` equals the first non-whitespace token, no token is a pure whitespace string, `&`-suffix implies return value 1 and no `&` in `argv`, non-`&` suffix implies return value 0
    - Tag: `Feature: tiny-shell, Property 1: parseline comprehensive behavior`
    - **Validates: Requirements 1.2, 2.2, 2.3**

  - [ ]* 4.3 Write property test for parseline whitespace-only rejection (Property 2)
    - Test file: `tests/property/prop_parseline.c`
    - **Property 2: parseline whitespace line rejection**
    - Generate strings composed entirely of spaces and tabs (including the empty string); assert `argv[0] == NULL` after the call
    - Tag: `Feature: tiny-shell, Property 2: parseline whitespace line rejection`
    - **Validates: Requirements 1.3**

  - [ ]* 4.4 Write property test for parseline over-limit rejection (Property 3)
    - Test file: `tests/property/prop_parseline.c`
    - **Property 3: parseline over-limit rejection**
    - Generate cmdlines with length > MAXLINE or with more than MAXARGS tokens; assert return value is `-1` and `argv[0]` is unchanged / null
    - Tag: `Feature: tiny-shell, Property 3: parseline over-limit rejection`
    - **Validates: Requirements 1.6**

- [ ] 5. Checkpoint — job manager and parseline
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 6. Implement `main` and install signal handlers
  - [ ] 6.1 Implement `main` in `tsh.c`
    - Install signal handlers: `Signal(SIGINT, sigint_handler)`, `Signal(SIGTSTP, sigtstp_handler)`, `Signal(SIGCHLD, sigchld_handler)` using the wrapper from the CSAPP textbook (`Signal` wraps `sigaction` with `SA_RESTART`)
    - Call `initjobs(jobs)`
    - Loop: print `tsh> ` to stdout (`fflush`), read a line with `fgets`; on EOF call `exit(0)`; otherwise call `eval(cmdline)`
    - _Requirements: 1.1, 1.4_

- [ ] 7. Implement `builtin_cmd` for `quit` and `jobs`
  - [ ] 7.1 Implement `builtin_cmd` in `tsh.c`
    - If `argv[0]` equals `"quit"`: call `exit(0)`
    - If `argv[0]` equals `"jobs"`: call `listjobs(jobs)` and return 1
    - Return 0 for any unrecognized command (non-built-in)
    - _Requirements: 4.1, 4.2, 4.3, 5.1, 5.2_

- [ ] 8. Implement `waitfg`
  - [ ] 8.1 Implement `waitfg` in `tsh.c`
    - Loop: if `getjobpid(jobs, pid) != NULL` and `job->state == FG`, call `sleep(1)` and recheck
    - Return when the job disappears from the list or its state changes away from FG
    - Do NOT call `waitpid` inside this function; all job removal and state transitions are handled exclusively by `sigchld_handler`
    - _Requirements: 12.1, 12.2, 12.3, 12.4_

- [ ] 9. Implement `eval` with fork/execve and race condition protection
  - [ ] 9.1 Implement core `eval` in `tsh.c`
    - Call `parseline(cmdline, argv)`; return immediately if `argv[0] == NULL` or parseline returns -1
    - Call `builtin_cmd(argv)`; if it returns 1, return immediately
    - Fork a child process; in the child: call `setpgid(0, 0)`, then `execve(argv[0], argv, environ)`; on execve failure print `"%s: Command not found\n"` to stderr and call `exit(1)`
    - In the parent: call `addjob(jobs, pid, bg ? BG : FG, cmdline)` to register the new job
    - If `bg == 1`: print `"[%d] (%d) %s\n"` with JID, PID, and cmdline; if `bg == 0`: call `waitfg(pid)`
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 11.1_

  - [ ] 9.2 Add SIGCHLD race condition protection to `eval` in `tsh.c`
    - Before `fork()`: call `sigprocmask(SIG_BLOCK, &mask_chld, &prev)` where `mask_chld` contains only SIGCHLD
    - In the child immediately after fork: call `sigprocmask(SIG_SETMASK, &prev, NULL)` before `setpgid`/`execve` so the child inherits no artificially blocked signals
    - In the parent after `addjob` completes: call `sigprocmask(SIG_SETMASK, &prev, NULL)` to restore the mask and allow pending SIGCHLD delivery
    - _Requirements: 10.1, 10.2, 10.3, 10.4_

- [ ] 10. Implement `sigchld_handler`
  - [ ] 10.1 Implement `sigchld_handler` in `tsh.c`
    - Save `errno` on entry; restore it before returning
    - Loop: `pid = waitpid(-1, &status, WNOHANG | WUNTRACED)` until `pid <= 0`
    - If `WIFEXITED(status)`: call `deletejob(jobs, pid)`
    - If `WIFSIGNALED(status)`: print `"Job [%d] (%d) terminated by signal %d\n"` then call `deletejob(jobs, pid)`
    - If `WIFSTOPPED(status)`: set `job->state = ST`; print `"Job [%d] (%d) stopped by signal %d\n"`
    - If the PID is not found in the job list, silently ignore it
    - _Requirements: 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 3.6, 8.4_

- [ ] 11. Implement `sigint_handler` and `sigtstp_handler`
  - [ ] 11.1 Implement both signal handlers in `tsh.c`
    - `sigint_handler(sig)`: call `fgpid(jobs)`; if the result is non-zero call `kill(-pid, SIGINT)`
    - `sigtstp_handler(sig)`: call `fgpid(jobs)`; if the result is non-zero call `kill(-pid, SIGTSTP)`
    - Both handlers must use only async-signal-safe functions; no `printf` — use `write` or `sio_puts` from the CSAPP csapp.c helper if needed
    - _Requirements: 7.1, 7.2, 7.3, 8.1, 8.2, 8.3, 11.2, 11.4_

- [ ] 12. Implement `do_bgfg` and wire `bg`/`fg` into `builtin_cmd`
  - [ ] 12.1 Implement `do_bgfg` in `tsh.c`
    - If `argv[1] == NULL`: print `"%s command requires PID or %%jobid argument\n"` and return
    - If `argv[1]` starts with `%`: parse the JID with `atoi(argv[1]+1)` and look up via `getjobjid`; if not found print `"%%%d: No such job\n"` and return
    - Else if `argv[1]` consists entirely of digits: parse the PID with `atoi(argv[1])` and look up via `getjobpid`; if not found print `"(%d): No such process\n"` and return
    - Else: print `"%s: argument must be a PID or %%jobid\n"` and return
    - Send `kill(-(job->pid), SIGCONT)` to the job's process group
    - For `fg`: set `job->state = FG` then call `waitfg(job->pid)`
    - For `bg`: set `job->state = BG` then print `"[%d] (%d) %s\n"` with JID, PID, and cmdline
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 3.6, 3.7, 11.3_

  - [ ] 12.2 Wire `bg` and `fg` into `builtin_cmd` in `tsh.c`
    - Add a branch in `builtin_cmd`: if `argv[0]` equals `"bg"` or `"fg"`, call `do_bgfg(argv)` and return 1
    - _Requirements: 6.1, 6.2_

  - [ ]* 12.3 Write property test for stop/resume state round trip (Property 8)
    - Test file: `tests/property/prop_joblist.c`
    - **Property 8: stop/resume state round trip**
    - For any job in BG or FG state, simulate SIGTSTP by setting `state = ST`; then simulate `bg`/`fg` resume by setting `state = BG` or `FG`; assert the resulting state is Running and the reverse transition is consistent
    - Tag: `Feature: tiny-shell, Property 8: stop/resume state round trip`
    - **Validates: Requirements 3.6, 3.7, 6.1, 6.2**

  - [ ]* 12.4 Write property test for `do_bgfg` invalid format rejection (Property 9)
    - Test file: `tests/property/prop_dobgfg.c`
    - **Property 9: do_bgfg invalid format rejection**
    - Generate argument strings that are neither pure positive-integer strings nor `%`-prefixed positive-integer strings (letters, negative numbers, empty string, special characters); assert `do_bgfg` prints a format error, does not modify the job list, and does not invoke `kill`
    - Tag: `Feature: tiny-shell, Property 9: do_bgfg invalid format rejection`
    - **Validates: Requirements 6.4**

- [ ] 13. Checkpoint — all unit and property tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 14. Integration testing with sdriver.pl trace files
  - [ ]* 14.1 Run all 16 trace files against tsh and verify output matches tshref
    - For each trace file `traceNN.txt` (01–16), run: `perl sdriver.pl -t traceNN.txt -s ./tsh -a "-p"` and compare output to `perl sdriver.pl -t traceNN.txt -s ./tshref -a "-p"`
    - All 16 traces must produce identical output (modulo PIDs, which sdriver normalises)
    - _Requirements: 1–12 (full system integration)_

- [ ] 15. Final checkpoint — all traces pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for a faster MVP; the shell will still be functionally correct without them
- The `theft` PBT library must be compiled alongside each property test file; add a `tests/property/Makefile` that links against `tsh.c`'s job manager and parseline functions via a testable compilation unit (e.g. compile `tsh.c` with `-DTESTING` to suppress `main`)
- All signal handlers must save and restore `errno`, and use only async-signal-safe functions; prefer `write(STDOUT_FILENO, ...)` or `sio_puts` over `printf` inside handlers
- The SIGCHLD blocking window in `eval` is the single most critical correctness invariant: the sequence must always be **BLOCK → `fork` → `addjob` → UNBLOCK**; violating this order creates a race where `deletejob` runs before `addjob`
- `waitfg` must never call `waitpid`; the only function responsible for reaping children and removing jobs from the list is `sigchld_handler`
- `nextjid` is a monotonically increasing counter; deleted JIDs are never reused
- sdriver.pl normalises PIDs in output comparison, so PID mismatches between tsh and tshref do not cause trace failures

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["2.1"] },
    { "id": 1, "tasks": ["3.1"] },
    { "id": 2, "tasks": ["3.2", "3.3*"] },
    { "id": 3, "tasks": ["4.1", "3.4*"] },
    { "id": 4, "tasks": ["7.1", "3.5*", "4.2*"] },
    { "id": 5, "tasks": ["6.1", "3.6*", "4.3*"] },
    { "id": 6, "tasks": ["8.1", "4.4*"] },
    { "id": 7, "tasks": ["9.1"] },
    { "id": 8, "tasks": ["9.2"] },
    { "id": 9, "tasks": ["10.1"] },
    { "id": 10, "tasks": ["11.1"] },
    { "id": 11, "tasks": ["12.1"] },
    { "id": 12, "tasks": ["12.2", "12.3*", "12.4*"] },
    { "id": 13, "tasks": ["14.1*"] }
  ]
}
```
