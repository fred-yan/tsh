# tsh 信号机制详解

> 本文基于 CSAPP Shell Lab（tiny shell）的实现，系统讲解 Unix 信号机制在 shell 中的应用，
> 并配以生活类比帮助理解抽象概念。

---

## 一、信号是什么

信号是 Unix 内核向进程发送的**异步通知**，本质是一个整数编号。

**生活类比：手机通知**

你正在专心打游戏（进程运行），突然屏幕顶部弹出一条系统通知（信号到达）。
你可以：

- 点进去处理 → 自定义处理函数（`signal()` / `sigaction()`）
- 滑掉忽略 → `SIG_IGN`
- 什么都不做，用系统默认响应 → 默认行为（终止 / 停止 / 忽略）

关键特性：**通知随时可能弹，不管你当时在干什么。** 这就是"异步"的含义。

### tsh 关心的三个信号

| 信号 | 编号 | 默认行为 | 触发来源 |
|------|------|----------|----------|
| `SIGINT` | 2 | 终止进程 | 用户按 Ctrl-C |
| `SIGTSTP` | 20 | 停止进程 | 用户按 Ctrl-Z |
| `SIGCHLD` | 17 | 忽略 | 子进程状态变化时内核自动发给父进程 |

---

## 二、三个信号处理函数详解

### 2.1 SIGINT — 紧急叫停令

**生活类比：工厂紧急停机按钮**

> 老板（用户）按下红色停机按钮（Ctrl-C）
> → 广播响了，但只有**车间主任**（tsh）的对讲机收到了信号
> → 车间主任通过内部对讲机转发停机令给**他负责的那个施工队**（`kill(-pgid, SIGINT)`）
> → 整个施工队停下来

**为什么老板不直接通知工人？**
因为老板不知道今天是哪个队在干活，车间主任才知道，所以需要他转达。

**为什么通知整个施工队而不是只通知队长？**
因为队长（子进程）可能还带着临时工（孙进程）。只叫停队长，临时工还在干，活儿停不了。
这正是用 `kill(-pgid, SIGINT)` 而非 `kill(pid, SIGINT)` 的原因——负号表示发给整个进程组。

```c
void sigint_handler(int sig) {
    pid_t pid = fgpid(jobs);   // 找到当前前台作业的 PID
    if (pid != 0)
        kill(-pid, SIGINT);    // 负号：发给整个进程组，而非单个进程
}
```

---

### 2.2 SIGTSTP — 暂停令，稍后继续

**生活类比：导演喊"卡！"**

> 剧组正在拍一场戏（前台作业运行中）
> → 导演（用户）喊了声"卡！"（Ctrl-Z）
> → 副导演（tsh 的 `sigtstp_handler`）把指令传达给所有演员（`kill(-pgid, SIGTSTP)`）
> → 所有演员原地停住，保持现有状态等待
> → 副导演回到监视器前继续工作（tsh 返回提示符）

**与 SIGINT 的本质区别：**

| | SIGINT | SIGTSTP |
|---|---|---|
| 比喻 | 导演说"这条作废，大家解散" | 导演说"先停一下，等会儿继续拍" |
| 结果 | 进程终止（不可恢复） | 进程暂停（可恢复） |
| 后续 | 无 | 可用 `fg`/`bg` + SIGCONT 恢复 |

```c
void sigtstp_handler(int sig) {
    pid_t pid = fgpid(jobs);
    if (pid != 0)
        kill(-pid, SIGTSTP);   // 停止整个前台进程组
}
```

暂停后的恢复流程：

```
tsh> fg %1    →  发 SIGCONT 给进程组，拉回前台继续运行
tsh> bg %1    →  发 SIGCONT 给进程组，在后台继续运行
```

---

### 2.3 SIGCHLD — 员工完工汇报

**生活类比：外卖送达后的系统推送**

你（tsh）同时派出了好几个外卖小哥（子进程）送餐。
送完了，系统自动给你推一条"已送达"通知（SIGCHLD）。
**你不需要一直打电话问他们到哪了**，送完了自然告诉你。

SIGCHLD 不只在子进程退出时触发，还涵盖三种状态变化：

| 子进程状态 | 对应宏 | tsh 的处理 |
|----------|--------|----------|
| 正常退出 | `WIFEXITED` | 从作业列表删除（`deletejob`） |
| 被信号杀死（如 SIGINT） | `WIFSIGNALED` | 打印终止消息，删除作业 |
| 被信号停止（如 SIGTSTP） | `WIFSTOPPED` | 更新状态为 ST（停止中） |

```c
void sigchld_handler(int sig) {
    int olderrno = errno;   // 必须保存，防止污染主流程的 errno
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        struct job_t *job = getjobpid(jobs, pid);
        if (job == NULL) continue;   // 未知 PID，忽略

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
```

---

## 三、waitpid、WNOHANG 与循环

### 3.1 参数含义

`waitpid(-1, &status, WNOHANG)` 三个参数各司其职：

| 参数 | 含义 |
|------|------|
| `-1` | 等待**任意**子进程（不指定 PID） |
| `&status` | 接收子进程退出状态的变量 |
| `WNOHANG` | **行为开关**：没有可回收的子进程时，不阻塞，立刻返回 |

### 3.2 返回值才是循环判断的依据

`WNOHANG` 是传给函数的输入指令，**不是**被判断的状态，系统也不会修改它。
循环判断的是 `waitpid` 的**返回值**：

```
返回值 > 0   →  成功回收了一个子进程，返回值即该子进程的 PID  →  继续循环
返回值 == 0  →  当前没有可回收的子进程（WNOHANG 直接返回）    →  退出循环
返回值 == -1 →  出错，或根本没有子进程                        →  退出循环
```

**生活类比：服务员收盘子**

- **不加 WNOHANG（阻塞）**：走到一桌，这桌没吃完，就站在旁边傻等。整个 tsh 被冻住。
- **加了 WNOHANG（非阻塞）**：走到一桌，看一眼——吃完了就收走（返回 PID > 0），没吃完就走开（返回 0），不等。然后遍历下一桌。

循环在遍历所有桌子，`> 0` 是"这桌有盘子可以收"的条件，返回 `0` 意味着转了一圈没有新的可收了，收工。

### 3.3 为什么必须用循环而不是单次调用

这与下面的信号 pending 机制直接相关。

---

## 四、sigprocmask 与 pending 信号

### 4.1 为什么需要屏蔽信号

**生活类比：HR 办理入职时把前台电话转到留言**

`eval` 函数在 `fork` 后需要调用 `addjob` 把新子进程登记到作业列表。
如果子进程跑得很快，在 `addjob` 执行之前就退出了，内核会立刻发 SIGCHLD 给 tsh：

```
fork() → 子进程立即退出 → SIGCHLD 到达
sigchld_handler 运行 → deletejob(pid)    ← 作业还没登记！
父进程才执行 addjob(pid)                 ← 登记了一个永远不会被删的"幽灵作业"
```

结果：`waitfg` 永远等不到这个作业消失，tsh 卡死。

### 4.2 解决方案：fork 前屏蔽，addjob 后解除

```c
sigset_t mask_chld, prev;
sigemptyset(&mask_chld);
sigaddset(&mask_chld, SIGCHLD);

sigprocmask(SIG_BLOCK, &mask_chld, &prev);  // 1. 屏蔽 SIGCHLD（手机静音）

pid_t pid = fork();

if (pid == 0) {                              // 子进程
    sigprocmask(SIG_SETMASK, &prev, NULL);  // 2. 子进程恢复信号掩码
    setpgid(0, 0);
    execve(argv[0], argv, environ);
}

// 父进程
addjob(jobs, pid, state, cmdline);          // 3. 登记完毕（手续办完）
sigprocmask(SIG_SETMASK, &prev, NULL);      // 4. 解除屏蔽（恢复接电话）
```

屏蔽期间 SIGCHLD 到达，不会被立即处理，而是进入"pending（待处理）"状态。

### 4.3 pending 信号的保存与投递

内核为每个进程维护两个位图：

```
blocked 位图：  哪些信号被屏蔽了（sigprocmask 控制）
pending 位图：  哪些信号已到达但还没处理

投递条件：pending 中为 1  且  blocked 中为 0
```

屏蔽期间信号到达 → `pending` 对应位置 1，但不投递。
解除屏蔽后 → 内核检查 `pending`，发现有 → 立即投递 → `sigchld_handler` 触发。

**生活类比：留言板**

前台（内核）把"有人来过电话"记在留言板上（pending = 1）。
你手续办完、开机后（解除屏蔽），看到留言板有条子，立刻回电话（投递信号）。

### 4.4 关键陷阱：信号不排队，只记"有没有"

**留言板只有一张纸，不是一叠。**

如果屏蔽期间 3 个子进程相继退出，发来了 3 次 SIGCHLD——
pending 位图里 SIGCHLD 那一位只能是 0 或 1，**后两个信号丢失了**，
解除屏蔽后只投递 1 次 `sigchld_handler`。

```
子进程A退出 → SIGCHLD pending = 1
子进程B退出 → SIGCHLD pending 仍 = 1（已经是1，无法叠加）
子进程C退出 → SIGCHLD pending 仍 = 1
解除屏蔽   → 只触发一次 sigchld_handler
```

**这正是 `sigchld_handler` 必须用 `while` 循环的根本原因：**

```c
// ❌ 错误：只处理一个子进程，其余僵尸进程无人回收
pid = waitpid(-1, &status, WNOHANG);

// ✅ 正确：一次性把所有已退出的子进程全部回收
while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    // ...
}
```

信号的不可靠性（不排队）和 `sigchld_handler` 的循环设计是一对配套的"问题—解法"。

---

## 五、进程组隔离

### 5.1 为什么要 setpgid(0, 0)

**生活类比：公司大群 vs. 独立项目群**

默认情况下，tsh 和它启动的所有子进程在同一个"公司大群"（终端前台进程组）里。
用户按 Ctrl-C，SIGINT 发给整个公司大群，tsh 自己也收到了，直接退出——这显然不对。

解决办法：每个子进程启动后第一件事就是**自己建一个新群**（`setpgid(0, 0)`），
群名（PGID）就是自己的员工号（PID）。

```c
// 子进程中，execve 之前执行
setpgid(0, 0);  // 进入以自己 PID 为 PGID 的新进程组
```

这样：
- tsh 在老群里，不受新群消息影响
- tsh 想联系某个项目组时，精准 @ 那个群（`kill(-pgid, sig)`），不误伤自己

### 5.2 负号的含义

```c
kill(-pid, SIGINT);   // 负号：发给 PGID == pid 的整个进程组
kill( pid, SIGINT);   // 无负号：只发给 PID == pid 的单个进程
```

用负号确保信号送达作业的所有子孙进程，而不只是直接子进程。

---

## 六、waitfg 与 sigchld_handler 的协作

### 6.1 waitfg 的设计

**生活类比：妈妈每隔一分钟看一眼孩子写没写完作业**

```c
void waitfg(pid_t pid) {
    while (getjobpid(jobs, pid) != NULL &&
           getjobpid(jobs, pid)->state == FG) {
        sleep(1);   // 每隔1秒看一眼，不是一直盯着
    }
}
```

`waitfg` **不调用 `waitpid`**，它只负责"等"。
谁负责把作业从列表里删掉或改状态？`sigchld_handler`。

### 6.2 两者的分工

```
waitfg          sigchld_handler
───────         ───────────────
等待（轮询）     实际回收（删除/改状态）
检查列表         修改列表
不调用 waitpid  调用 waitpid
```

孩子写完了（子进程退出）→ 推送通知（SIGCHLD）→ `sigchld_handler` 把作业从列表里划掉（`deletejob`）→ 妈妈下次一看，作业本没了（`getjobpid` 返回 NULL）→ `waitfg` 返回 → tsh 显示提示符。

---

## 七、async-signal-safe 约束

信号处理函数可以在程序执行的**任意时刻**被打断，包括正在执行 `malloc` 或 `printf` 的中途。
如果处理函数里再调用这些非线程安全的函数，可能导致死锁（重入内部锁）。

**规则：信号处理函数中只能使用 async-signal-safe 函数。**

常见 async-signal-safe 函数：`write`、`waitpid`、`kill`、`_exit`、简单赋值。

两个必须遵守的习惯：

```c
void sigchld_handler(int sig) {
    int olderrno = errno;   // ① 保存 errno，防止污染主流程
    // ... 处理逻辑 ...
    errno = olderrno;       // ② 恢复 errno
}
```

---

## 八、完整信号流程总览

```
用户操作          信号流向                       tsh 内部结果
──────────────────────────────────────────────────────────────────
Ctrl-C       → SIGINT → sigint_handler
                        → kill(-pgid, SIGINT)
                        → 子进程退出
                        → SIGCHLD → sigchld_handler
                                    → WIFSIGNALED → 打印终止消息
                                    → deletejob
                                    → waitfg 检测到作业消失，返回

Ctrl-Z       → SIGTSTP → sigtstp_handler
                         → kill(-pgid, SIGTSTP)
                         → 子进程停止
                         → SIGCHLD → sigchld_handler
                                     → WIFSTOPPED → state = ST
                                     → waitfg 检测到状态非FG，返回

fg/bg 命令   → SIGCONT (kill) → 子进程恢复运行
                                 (fg) → waitfg 重新等待
                                 (bg) → 立即返回提示符

子进程正常退出 → SIGCHLD → sigchld_handler
                           → WIFEXITED → deletejob
                           → waitfg 返回（若是前台作业）
```

SIGINT 和 SIGTSTP 是**输入端**——把用户意图转化为进程组信号；
SIGCHLD 是**输出端**——把子进程状态变化同步回 tsh 的作业列表；
`waitfg` 夹在中间，依赖 SIGCHLD 处理结果决定是否继续等待。
