# Requirements Document

## Introduction

Tiny Shell（tsh）是 CSAPP Shell Lab 中需要实现的一个简化 Unix shell。tsh 支持前台与后台作业管理、内置命令以及 POSIX 信号处理，目标平台为 Linux，使用 C 语言实现。tsh 不支持管道或 I/O 重定向，专注于进程创建、作业控制与信号处理的核心机制。

## Glossary

- **tsh**：Tiny Shell，本项目实现的 shell 程序主体。
- **Job（作业）**：由 tsh 启动的一个外部命令实例，对应一个子进程。
- **JID（作业号）**：tsh 为每个作业分配的正整数编号，从 1 开始递增，在命令行中以 `%` 前缀表示（如 `%1`）。
- **PID（进程号）**：操作系统分配给进程的唯一整数标识符。
- **Foreground_Job（前台作业）**：tsh 当前正在等待其完成的作业；同一时刻最多只有一个前台作业。
- **Background_Job（后台作业）**：独立运行、tsh 不等待其完成的作业。
- **Job_List（作业列表）**：tsh 维护的所有活跃前台与后台作业的数据结构。
- **Job_Manager（作业管理器）**：tsh 内部负责维护 Job_List 并提供 addjob/deletejob/getjob 操作的模块。
- **Process_Group（进程组）**：共享同一 PGID 的一组进程；tsh 通过向进程组发送信号来控制作业的所有子进程。
- **sigchld_handler**：处理 SIGCHLD 信号、回收僵尸子进程的信号处理函数。
- **sigint_handler**：处理 SIGINT 信号（Ctrl-C）的信号处理函数。
- **sigtstp_handler**：处理 SIGTSTP 信号（Ctrl-Z）的信号处理函数。
- **waitfg_function**：在前台作业运行期间阻塞 tsh 主循环的等待函数。
- **Stopped_State（停止状态）**：作业因收到 SIGTSTP 而暂停执行的状态。
- **Running_State（运行状态）**：作业正在正常执行的状态。

## Requirements

### 需求 1：Shell 提示符与命令行解析

**用户故事：** 作为用户，我希望 tsh 显示提示符并读取命令，以便与 shell 进行交互。

#### 验收标准

1. THE tsh SHALL display the prompt string `tsh> ` to stdout before reading each command line.
2. WHEN a command line is entered, THE tsh SHALL parse it into a command name and zero or more arguments, treating sequences of one or more space or tab characters as delimiters between tokens.
3. WHEN an empty command line (zero characters or containing only spaces and tabs) is entered, THE tsh SHALL redisplay the prompt without executing any command.
4. WHEN EOF is received on stdin, THE tsh SHALL exit with status 0.
5. THE tsh SHALL support command lines of at most 1024 characters (MAXLINE) and at most 128 arguments (MAXARGS).
6. IF a command line exceeds 1024 characters or contains more than 128 arguments, THEN THE tsh SHALL display an error message indicating which limit was exceeded and redisplay the prompt without executing any command.

---

### 需求 2：外部命令执行

**用户故事：** 作为用户，我希望 tsh 能执行外部程序，以便运行任意命令。

#### 验收标准

1. IF a command name is not a built-in command, THEN THE tsh SHALL fork a child process and execute the command via execve, searching the PATH environment variable if the command is not an absolute or relative path.
2. IF a command line does not end with `&`, THEN THE tsh SHALL run the command as a Foreground_Job and wait for it to complete before displaying the next prompt.
3. IF a command line ends with `&`, THEN THE tsh SHALL strip the `&` token from the argument list, run the command as a Background_Job, and immediately display the next prompt without waiting.
4. WHEN a Background_Job is started, THE tsh SHALL print to stdout a message in the format `[JID] (PID) command_line\n`, where JID is the job's sequence number starting from 1, PID is the process ID, and command_line is the original input with the trailing `&` token removed.
5. IF execve fails, THEN THE tsh SHALL print an error message to stderr indicating the command name that failed and terminate the child process with exit status 1.

---

### 需求 3：作业管理

**用户故事：** 作为用户，我希望 tsh 跟踪所有活跃作业，以便监控和控制后台进程。

#### 验收标准

1. THE Job_Manager SHALL assign a unique JID to each new job, starting from 1 and incrementing by 1 for each subsequent job; JIDs from removed jobs are never recycled.
2. WHEN a new job is created, THE Job_Manager SHALL store that job's JID, PID, initial state of Running_State, and original command line (up to 512 characters) in the Job_List.
3. WHEN a job terminates or is reaped, THE Job_Manager SHALL remove that job from the Job_List.
4. THE tsh SHALL accept both a bare PID (e.g., `1234`) and a JID prefixed with `%` (e.g., `%2`) as valid job identifiers in all commands that accept a job argument.
5. IF a requested job identifier does not match any entry in the Job_List, THEN THE tsh SHALL print an error message indicating the job was not found.
6. WHEN a job receives a stop signal, THE Job_Manager SHALL update that job's state from Running_State to Stopped_State.
7. WHEN a stopped job is resumed via SIGCONT, THE Job_Manager SHALL update that job's state from Stopped_State to Running_State.

---

### 需求 4：内置命令 quit

**用户故事：** 作为用户，我希望通过内置命令退出 shell，以便结束会话。

#### 验收标准

1. WHEN the command `quit` is entered, THE tsh SHALL terminate the shell process with exit status 0.
2. WHEN the `quit` command is entered with additional arguments, THE tsh SHALL still terminate the shell process with exit status 0 and ignore the extra arguments.
3. WHEN a background job is running and `quit` is entered, THE tsh SHALL terminate the shell process with exit status 0 without waiting for background jobs to complete.

---

### 需求 5：内置命令 jobs

**用户故事：** 作为用户，我希望列出所有后台作业，以便了解当前运行情况。

#### 验收标准

1. WHEN the command `jobs` is entered, THE tsh SHALL list all jobs currently in the Job_List to stdout. If the Job_List is empty, THE tsh SHALL display nothing and return to the prompt.
2. WHEN listing jobs, THE tsh SHALL display each job's JID, PID, state (one of: "Running" or "Stopped"), and command line in the format `[JID] (PID) State command_line`.

---

### 需求 6：内置命令 bg 与 fg

**用户故事：** 作为用户，我希望在前台和后台之间切换作业，以便灵活控制作业执行。

#### 验收标准

1. WHEN `bg <job>` is entered, THE tsh SHALL send SIGCONT to the Process_Group of `<job>`, update the job's state to Running_State, and return the shell prompt without waiting for the job to finish.
2. WHEN `fg <job>` is entered, THE tsh SHALL send SIGCONT to the Process_Group of `<job>`, update the job state to Running_State, and wait for the job until the job terminates or transitions to Stopped_State.
3. IF `bg` or `fg` is entered without a job argument, THEN THE tsh SHALL print an error message stating that the argument is required.
4. IF `bg` or `fg` is entered with an argument that is neither a positive integer (PID) nor a `%` character followed by a positive integer (%JID), THEN THE tsh SHALL print an error message describing the invalid argument format.
5. IF `bg` or `fg` is entered with a valid-format argument that does not match any job in the Job_List, THEN THE tsh SHALL print an error message indicating the job was not found.

---

### 需求 7：SIGINT 信号处理（Ctrl-C）

**用户故事：** 作为用户，我希望 Ctrl-C 终止当前前台作业，以便中断运行中的程序。

#### 验收标准

1. WHEN the user presses Ctrl-C, THE sigint_handler SHALL send SIGINT to the entire Process_Group of the current Foreground_Job using the negated PGID.
2. IF there is no Foreground_Job when Ctrl-C is pressed, THEN THE sigint_handler SHALL return without sending any signal.
3. THE sigint_handler SHALL use only async-signal-safe functions in its implementation.

---

### 需求 8：SIGTSTP 信号处理（Ctrl-Z）

**用户故事：** 作为用户，我希望 Ctrl-Z 暂停当前前台作业，以便稍后恢复执行。

#### 验收标准

1. WHEN the user presses Ctrl-Z, THE sigtstp_handler SHALL send SIGTSTP to the entire Process_Group of the current Foreground_Job using the negated PGID.
2. IF there is no Foreground_Job when Ctrl-Z is pressed, THEN THE sigtstp_handler SHALL return without sending any signal.
3. THE sigtstp_handler SHALL use only async-signal-safe functions in its implementation.
4. WHEN the Foreground_Job's process group receives SIGTSTP, THE Job_Manager SHALL update the corresponding job's state to Stopped_State so that the job can later be resumed via `fg` or `bg`.

---

### 需求 9：SIGCHLD 处理与僵尸进程回收

**用户故事：** 作为开发者，我希望所有僵尸子进程被及时回收，以便系统不积累僵尸进程。

#### 验收标准

1. WHEN a child process changes state, THE sigchld_handler SHALL call waitpid with flags `WNOHANG | WUNTRACED` in a loop to reap all available zombie children without blocking.
2. WHEN a child process terminates normally, THE sigchld_handler SHALL remove the corresponding job from the Job_List.
3. IF a child process is terminated by an uncaught signal, THEN THE sigchld_handler SHALL print a message to stdout containing the job's PID and a name or description of the terminating signal, then remove the job from the Job_List.
4. WHEN a child process is stopped by a signal, THE sigchld_handler SHALL update the corresponding job's state to Stopped_State and print a notification message to stdout containing the JID, PID, and command line.
5. THE sigchld_handler SHALL use only async-signal-safe functions in its implementation.
6. IF sigchld_handler receives a state change for a PID not present in the Job_List, THEN THE sigchld_handler SHALL silently ignore that state change without printing any message or modifying the Job_List.

---

### 需求 10：竞态条件防护

**用户故事：** 作为开发者，我希望 tsh 避免 fork 与信号处理之间的竞态条件，以便作业始终被正确追踪。

#### 验收标准

1. WHEN tsh is creating a new job, THE tsh SHALL call sigprocmask to block SIGCHLD before calling fork.
2. WHEN addjob completes after fork returns in the parent process, THE tsh SHALL call sigprocmask to unblock SIGCHLD.
3. WHEN the child process is created via fork, THE child process SHALL restore the signal mask saved before SIGCHLD was blocked before calling execve, ensuring SIGCHLD is unblocked and the executed program inherits no artificially blocked signals.
4. WHEN tsh calls sigprocmask to block SIGCHLD before fork, THE tsh SHALL save the current signal mask so that both the parent process and the child process can restore it to its pre-block state.

---

### 需求 11：进程组隔离

**用户故事：** 作为开发者，我希望每个子进程处于独立的进程组中，以便信号能正确传递给作业的所有子孙进程。

#### 验收标准

1. WHEN a child process is created via fork, THE child process SHALL call `setpgid(0, 0)` before calling execve to place itself in a new Process_Group with PGID equal to its own PID.
2. WHEN tsh receives SIGINT or SIGTSTP while a Foreground_Job exists in the Job_List, THE tsh SHALL send the received signal to the Foreground_Job's process group using the negated PGID.
3. WHEN the bg or fg command is executed for a job, THE tsh SHALL send SIGCONT to the job's process group using the negated PGID.
4. IF tsh receives SIGINT or SIGTSTP while no Foreground_Job exists in the Job_List, THEN THE tsh SHALL ignore the signal without performing any action.

---

### 需求 12：前台作业等待

**用户故事：** 作为开发者，我希望 tsh 正确等待前台作业完成，以便在作业结束前不显示新提示符。

#### 验收标准

1. WHILE the job with the given PID is present in the Job_List with Foreground_Job state, THE waitfg_function SHALL sleep for 1 second per iteration before rechecking the Job_List.
2. WHEN the Foreground_Job terminates or transitions to Stopped_State, THE waitfg_function SHALL return control to the tsh main loop.
3. THE waitfg_function SHALL NOT use waitpid directly; job removal and state transitions are handled exclusively by sigchld_handler.
4. WHEN tsh creates a Foreground_Job, THE tsh SHALL NOT display a new command prompt until waitfg returns.
