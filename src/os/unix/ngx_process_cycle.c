
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_channel.h>

static void ngx_start_worker_processes(ngx_cycle_t *cycle, ngx_int_t n,
                                       ngx_int_t type);
static void ngx_start_cache_manager_processes(ngx_cycle_t *cycle,
                                              ngx_uint_t respawn);
static void ngx_pass_open_channel(ngx_cycle_t *cycle, ngx_channel_t *ch);
static void ngx_signal_worker_processes(ngx_cycle_t *cycle, int signo);
static ngx_uint_t ngx_reap_children(ngx_cycle_t *cycle);
static void ngx_master_process_exit(ngx_cycle_t *cycle);
static void ngx_worker_process_cycle(ngx_cycle_t *cycle, void *data);
static void ngx_worker_process_init(ngx_cycle_t *cycle, ngx_int_t worker);
static void ngx_worker_process_exit(ngx_cycle_t *cycle);
static void ngx_channel_handler(ngx_event_t *ev);
static void ngx_cache_manager_process_cycle(ngx_cycle_t *cycle, void *data);
static void ngx_cache_manager_process_handler(ngx_event_t *ev);
static void ngx_cache_loader_process_handler(ngx_event_t *ev);

ngx_uint_t ngx_process;
ngx_uint_t ngx_worker;
ngx_pid_t ngx_pid;

sig_atomic_t ngx_reap;
sig_atomic_t ngx_sigio;
sig_atomic_t ngx_sigalrm;
sig_atomic_t ngx_terminate;
sig_atomic_t ngx_quit;
sig_atomic_t ngx_debug_quit;
ngx_uint_t ngx_exiting;
sig_atomic_t ngx_reconfigure;
sig_atomic_t ngx_reopen;

sig_atomic_t ngx_change_binary;
ngx_pid_t ngx_new_binary;
ngx_uint_t ngx_inherited;
ngx_uint_t ngx_daemonized;

sig_atomic_t ngx_noaccept;
ngx_uint_t ngx_noaccepting;
ngx_uint_t ngx_restart;

static u_char master_process[] = "master process";

static ngx_cache_manager_ctx_t ngx_cache_manager_ctx = {
    ngx_cache_manager_process_handler, "cache manager process", 0};

static ngx_cache_manager_ctx_t ngx_cache_loader_ctx = {
    ngx_cache_loader_process_handler, "cache loader process", 60000};

static ngx_cycle_t ngx_exit_cycle;
static ngx_log_t ngx_exit_log;
static ngx_open_file_t ngx_exit_log_file;
/**
 * master进程主循环函数
 */
void ngx_master_process_cycle(ngx_cycle_t *cycle)
{
    char *title;
    u_char *p;
    size_t size;
    ngx_int_t i;
    ngx_uint_t n, sigio;
    sigset_t set;/* 信号集 */
    struct itimerval itv;
    ngx_uint_t live;
    ngx_msec_t delay;
    ngx_listening_t *ls;
    ngx_core_conf_t *ccf;

    sigemptyset(&set);//清空信号集 相当于初始化信号集 必须调用
    /* 将下列信号 添加到信号集中 */
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGIO);
    sigaddset(&set, SIGINT);
    sigaddset(&set, ngx_signal_value(NGX_RECONFIGURE_SIGNAL));
    sigaddset(&set, ngx_signal_value(NGX_REOPEN_SIGNAL));
    sigaddset(&set, ngx_signal_value(NGX_NOACCEPT_SIGNAL));
    sigaddset(&set, ngx_signal_value(NGX_TERMINATE_SIGNAL));
    sigaddset(&set, ngx_signal_value(NGX_SHUTDOWN_SIGNAL));
    sigaddset(&set, ngx_signal_value(NGX_CHANGEBIN_SIGNAL));
    /**
     * 设置信号屏蔽字
     * 参数1: 操作类型
     *        SIG_BLOCK    将set信号集与当前进程原有的信号屏蔽字，进行或操作，相当于交集。
     *        SIG_UNBLOCK  解除set指定的信号
     *        SIG_SETMASK  将当前进程信号屏蔽字设置为set信号集。相当于重新赋值
     * 参数2:
     * 参数3: 该参数是输出参数 返回当前进程设置的信号屏蔽字
     * 个人理解：
     * 信号忽略与信号屏蔽，两种不同行为：
     *    信号忽略： 指的是操作系统将信号发给进程，进程对信号的处理默认是行为是忽略
     *    信号屏蔽： 产生某个信号后，操作系统暂时不发给进程（有操作系统阻塞），等进程取消屏蔽后在发给
     *             进程。
     * 对于nginx来说，在主进程fork子进程之前要把所有信号调用sigprocmask阻塞住，
     *              等待fork成功后再将阻塞信号清除
     *
     * sigprocmask函数适用于单线程的进程
     * pthread_sigmask函数适用于多线程的进程
     */
    if (sigprocmask(SIG_BLOCK, &set, NULL) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "sigprocmask() failed");
    }

    sigemptyset(&set);

    size = sizeof(master_process);

    for (i = 0; i < ngx_argc; i++)
    {
        size += ngx_strlen(ngx_argv[i]) + 1;
    }

    title = ngx_pnalloc(cycle->pool, size);
    if (title == NULL)
    {
        /* fatal */
        exit(2);
    }

    p = ngx_cpymem(title, master_process, sizeof(master_process) - 1);
    for (i = 0; i < ngx_argc; i++)
    {
        *p++ = ' ';
        p = ngx_cpystrn(p, (u_char *)ngx_argv[i], size);
    }

    ngx_setproctitle(title);//设置master进程名称

    ccf = (ngx_core_conf_t *)ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    //启动worker进程
    ngx_start_worker_processes(cycle, ccf->worker_processes,
                               NGX_PROCESS_RESPAWN);
    ngx_start_cache_manager_processes(cycle, 0);//启动监控进程 默认不启动

    ngx_new_binary = 0;
    delay = 0;
    sigio = 0;
    live = 1; //表示是否活跃

    for (;;)
    {
        if (delay)
        {//延迟
            if (ngx_sigalrm)
            {
                sigio = 0;
                delay *= 2;
                ngx_sigalrm = 0;
            }

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                           "termination cycle: %M", delay);

            itv.it_interval.tv_sec = 0;
            itv.it_interval.tv_usec = 0;
            itv.it_value.tv_sec = delay / 1000;
            itv.it_value.tv_usec = (delay % 1000) * 1000;

            if (setitimer(ITIMER_REAL, &itv, NULL) == -1)
            {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                              "setitimer() failed");
            }
        }

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0, "sigsuspend");
        /**
         * 进程阻塞  非常重要一点
         * 等待信号发生 当信号产生会先调用信号处理函数 当信号处理函数结束后
         * sigsuspend才返回，执行后续代码
         */
        sigsuspend(&set);

        ngx_time_update();

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "wake up, sigio %i", sigio);

        if (ngx_reap)
        {//当子进程异常退出时，会接收到SIGCHLD信号，因此会在调用起来一个子进程
            ngx_reap = 0;
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0, "reap children");

            live = ngx_reap_children(cycle);
        }

        if (!live && (ngx_terminate || ngx_quit))
        {//立即退出
            ngx_master_process_exit(cycle);
        }

        if (ngx_terminate)
        {//接收到TERM信号 理应立即关闭 但是Nginx采用延迟关闭方式
            if (delay == 0)
            {
                delay = 50; //50ms
            }

            if (sigio)
            {
                sigio--;
                continue;
            }

            sigio = ccf->worker_processes + 2 /* cache processes */;

            if (delay > 1000)
            {//如果延迟大于1000ms 则暴力关闭进程
                ngx_signal_worker_processes(cycle, SIGKILL);
            }
            else
            {
                ngx_signal_worker_processes(cycle,
                                            ngx_signal_value(NGX_TERMINATE_SIGNAL));
            }

            continue;
        }

        if (ngx_quit)
        {//从容关闭
            ngx_signal_worker_processes(cycle,
                                        ngx_signal_value(NGX_SHUTDOWN_SIGNAL));

            ls = cycle->listening.elts;
            for (n = 0; n < cycle->listening.nelts; n++)
            {
                if (ngx_close_socket(ls[n].fd) == -1)
                {
                    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                                  ngx_close_socket_n " %V failed",
                                  &ls[n].addr_text);
                }
            }
            cycle->listening.nelts = 0;

            continue;
        }
        /**
         * 当master进程接收到HUP信号，用于重新加载配置。例如:配置文件变化，需要
         * 更新配置
         */
        if (ngx_reconfigure)
        {
            ngx_reconfigure = 0;

            if (ngx_new_binary)
            {
                ngx_start_worker_processes(cycle, ccf->worker_processes,
                                           NGX_PROCESS_RESPAWN);
                ngx_start_cache_manager_processes(cycle, 0);
                ngx_noaccepting = 0;

                continue;
            }

            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "reconfiguring");

            cycle = ngx_init_cycle(cycle);
            if (cycle == NULL)
            {
                cycle = (ngx_cycle_t *)ngx_cycle;
                continue;
            }

            ngx_cycle = cycle;
            ccf = (ngx_core_conf_t *)ngx_get_conf(cycle->conf_ctx,
                                                  ngx_core_module);
            ngx_start_worker_processes(cycle, ccf->worker_processes,
                                       NGX_PROCESS_JUST_RESPAWN);
            ngx_start_cache_manager_processes(cycle, 1);

            /* allow new processes to start */
            ngx_msleep(100);

            live = 1;
            ngx_signal_worker_processes(cycle,
                                        ngx_signal_value(NGX_SHUTDOWN_SIGNAL));
        }
        /**
         * 表示重启worker进程，进入此分支的前提是master进程接收到SIGCHLD信号，即
         * worker进程异常退出
         */
        if (ngx_restart)
        {
            ngx_restart = 0;
            ngx_start_worker_processes(cycle, ccf->worker_processes,
                                       NGX_PROCESS_RESPAWN);
            ngx_start_cache_manager_processes(cycle, 0);
            live = 1;
        }
        /**
         * 当master进程接收到USR1信号，表明需要重新打开日志文件
         */
        if (ngx_reopen)
        {
            ngx_reopen = 0;
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "reopening logs");
            ngx_reopen_files(cycle, ccf->user);
            ngx_signal_worker_processes(cycle,
                                        ngx_signal_value(NGX_REOPEN_SIGNAL));
        }
        /**
         * 当master进程接收到USR2信号，表明进行平滑升级
         */
        if (ngx_change_binary)
        {
            ngx_change_binary = 0;
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "changing binary");
            ngx_new_binary = ngx_exec_new_binary(cycle, ngx_argv);
        }
        /**
         * master进程收到WINCH信号(通过kill发送)后，会通过channel发送QUIT消息
         * 给worker进程,当worker进程接收到QUIT消息就会优雅退出
         */
        if (ngx_noaccept)
        {
            ngx_noaccept = 0;
            ngx_noaccepting = 1;
            ngx_signal_worker_processes(cycle,
                                        ngx_signal_value(NGX_SHUTDOWN_SIGNAL));
        }
    }
}

void ngx_single_process_cycle(ngx_cycle_t *cycle)
{
    ngx_uint_t i;

    if (ngx_set_environment(cycle, NULL) == NULL)
    {
        /* fatal */
        exit(2);
    }

    for (i = 0; cycle->modules[i]; i++)
    {
        if (cycle->modules[i]->init_process)
        {
            if (cycle->modules[i]->init_process(cycle) == NGX_ERROR)
            {
                /* fatal */
                exit(2);
            }
        }
    }

    for (;;)
    {
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0, "worker cycle");

        ngx_process_events_and_timers(cycle);

        if (ngx_terminate || ngx_quit)
        {

            for (i = 0; cycle->modules[i]; i++)
            {
                if (cycle->modules[i]->exit_process)
                {
                    cycle->modules[i]->exit_process(cycle);
                }
            }

            ngx_master_process_exit(cycle);
        }

        if (ngx_reconfigure)
        {
            ngx_reconfigure = 0;
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "reconfiguring");

            cycle = ngx_init_cycle(cycle);
            if (cycle == NULL)
            {
                cycle = (ngx_cycle_t *)ngx_cycle;
                continue;
            }

            ngx_cycle = cycle;
        }

        if (ngx_reopen)
        {
            ngx_reopen = 0;
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "reopening logs");
            ngx_reopen_files(cycle, (ngx_uid_t)-1);
        }
    }
}

/**
 * 创建worker进程
 * @param cycle  核心结构体
 * @param n      worker进程数量
 * @param type   创建worker进程方式
 */
static void
ngx_start_worker_processes(ngx_cycle_t *cycle, ngx_int_t n, ngx_int_t type)
{
    ngx_int_t i;
    ngx_channel_t ch;

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "start worker processes");

    ngx_memzero(&ch, sizeof(ngx_channel_t));

    ch.command = NGX_CMD_OPEN_CHANNEL; //发送第一个消息

    for (i = 0; i < n; i++)
    {
        //启动多个worker进程 回调函数为ngx_worker_process_cycle
        ngx_spawn_process(cycle, ngx_worker_process_cycle,
                          (void *)(intptr_t)i, "worker process", type);

        ch.pid = ngx_processes[ngx_process_slot].pid; /* 子进程id */
        ch.slot = ngx_process_slot; /* 子进程在ngx_processes数组中索引 */
        ch.fd = ngx_processes[ngx_process_slot].channel[0]; /* 父进程socketpair fd */

        ngx_pass_open_channel(cycle, &ch);//通过unix domain发送第一个消息给worker进程
    }
}

static void
ngx_start_cache_manager_processes(ngx_cycle_t *cycle, ngx_uint_t respawn)
{
    ngx_uint_t i, manager, loader;
    ngx_path_t **path;
    ngx_channel_t ch;

    manager = 0;
    loader = 0;

    path = ngx_cycle->paths.elts;
    for (i = 0; i < ngx_cycle->paths.nelts; i++)
    {

        if (path[i]->manager)
        {
            manager = 1;
        }

        if (path[i]->loader)
        {
            loader = 1;
        }
    }

    if (manager == 0)
    {
        return;
    }

    ngx_spawn_process(cycle, ngx_cache_manager_process_cycle,
                      &ngx_cache_manager_ctx, "cache manager process",
                      respawn ? NGX_PROCESS_JUST_RESPAWN : NGX_PROCESS_RESPAWN);

    ngx_memzero(&ch, sizeof(ngx_channel_t));

    ch.command = NGX_CMD_OPEN_CHANNEL;
    ch.pid = ngx_processes[ngx_process_slot].pid;
    ch.slot = ngx_process_slot;
    ch.fd = ngx_processes[ngx_process_slot].channel[0];

    ngx_pass_open_channel(cycle, &ch);

    if (loader == 0)
    {
        return;
    }

    ngx_spawn_process(cycle, ngx_cache_manager_process_cycle,
                      &ngx_cache_loader_ctx, "cache loader process",
                      respawn ? NGX_PROCESS_JUST_SPAWN : NGX_PROCESS_NORESPAWN);

    ch.command = NGX_CMD_OPEN_CHANNEL;
    ch.pid = ngx_processes[ngx_process_slot].pid;
    ch.slot = ngx_process_slot;
    ch.fd = ngx_processes[ngx_process_slot].channel[0];

    ngx_pass_open_channel(cycle, &ch);
}

static void
ngx_pass_open_channel(ngx_cycle_t *cycle, ngx_channel_t *ch)
{
    ngx_int_t i;

    for (i = 0; i < ngx_last_process; i++)
    {

        if (i == ngx_process_slot || ngx_processes[i].pid == -1 || ngx_processes[i].channel[0] == -1)
        {
            continue;
        }

        ngx_log_debug6(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                       "pass channel s:%i pid:%P fd:%d to s:%i pid:%P fd:%d",
                       ch->slot, ch->pid, ch->fd,
                       i, ngx_processes[i].pid,
                       ngx_processes[i].channel[0]);

        /* TODO: NGX_AGAIN */

        ngx_write_channel(ngx_processes[i].channel[0],
                          ch, sizeof(ngx_channel_t), cycle->log);
    }
}

/**
 * 向worker进程发送信号
 * @param cycle 核心结构体
 * @param signo 信号类型
 */
static void
ngx_signal_worker_processes(ngx_cycle_t *cycle, int signo)
{
    ngx_int_t i;
    ngx_err_t err;
    ngx_channel_t ch;

    ngx_memzero(&ch, sizeof(ngx_channel_t));

#if (NGX_BROKEN_SCM_RIGHTS)

    ch.command = 0;//windows 下

#else
    /* 根据信号类型 生成命令码 */
    switch (signo)
    {

    case ngx_signal_value(NGX_SHUTDOWN_SIGNAL): /* QUIT */
        ch.command = NGX_CMD_QUIT;
        break;

    case ngx_signal_value(NGX_TERMINATE_SIGNAL): /* TERM */
        ch.command = NGX_CMD_TERMINATE;
        break;

    case ngx_signal_value(NGX_REOPEN_SIGNAL): /* USR1 */
        ch.command = NGX_CMD_REOPEN;
        break;

    default:
        ch.command = 0; //其他信号
    }

#endif

    ch.fd = -1;

    for (i = 0; i < ngx_last_process; i++)
    {

        ngx_log_debug7(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "child: %i %P e:%d t:%d d:%d r:%d j:%d",
                       i,
                       ngx_processes[i].pid,
                       ngx_processes[i].exiting,
                       ngx_processes[i].exited,
                       ngx_processes[i].detached,
                       ngx_processes[i].respawn,
                       ngx_processes[i].just_spawn);

        if (ngx_processes[i].detached || ngx_processes[i].pid == -1)
        {//无效进程
            continue;
        }

        if (ngx_processes[i].just_spawn)
        {
            ngx_processes[i].just_spawn = 0;
            continue;
        }

        if (ngx_processes[i].exiting && signo == ngx_signal_value(NGX_SHUTDOWN_SIGNAL))
        {
            continue;
        }

        if (ch.command)
        {//如果command不为0 则表示通过channel(unix domain)方式发送消息
            if (ngx_write_channel(ngx_processes[i].channel[0],
                                  &ch, sizeof(ngx_channel_t), cycle->log) == NGX_OK)
            {
                if (signo != ngx_signal_value(NGX_REOPEN_SIGNAL))
                {//如果不是REOPEN则把进程退出标志设置为1
                    ngx_processes[i].exiting = 1;
                }

                continue;
            }
        }

        ngx_log_debug2(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                       "kill (%P, %d)", ngx_processes[i].pid, signo);
        /* 当command为0 使用kill发送信号 */
        if (kill(ngx_processes[i].pid, signo) == -1)
        {
            err = ngx_errno;
            ngx_log_error(NGX_LOG_ALERT, cycle->log, err,
                          "kill(%P, %d) failed", ngx_processes[i].pid, signo);

            if (err == NGX_ESRCH)
            {
                ngx_processes[i].exited = 1;
                ngx_processes[i].exiting = 0;
                ngx_reap = 1;
            }

            continue;
        }

        if (signo != ngx_signal_value(NGX_REOPEN_SIGNAL))
        {
            ngx_processes[i].exiting = 1;
        }
    }
}

static ngx_uint_t
ngx_reap_children(ngx_cycle_t *cycle)
{
    ngx_int_t i, n;
    ngx_uint_t live;
    ngx_channel_t ch;
    ngx_core_conf_t *ccf;

    ngx_memzero(&ch, sizeof(ngx_channel_t));

    ch.command = NGX_CMD_CLOSE_CHANNEL;
    ch.fd = -1;

    live = 0;
    for (i = 0; i < ngx_last_process; i++)
    {

        ngx_log_debug7(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "child: %i %P e:%d t:%d d:%d r:%d j:%d",
                       i,
                       ngx_processes[i].pid,
                       ngx_processes[i].exiting,
                       ngx_processes[i].exited,
                       ngx_processes[i].detached,
                       ngx_processes[i].respawn,
                       ngx_processes[i].just_spawn);

        if (ngx_processes[i].pid == -1)
        {
            continue;
        }

        if (ngx_processes[i].exited)
        {

            if (!ngx_processes[i].detached)
            {
                ngx_close_channel(ngx_processes[i].channel, cycle->log);

                ngx_processes[i].channel[0] = -1;
                ngx_processes[i].channel[1] = -1;

                ch.pid = ngx_processes[i].pid;
                ch.slot = i;

                for (n = 0; n < ngx_last_process; n++)
                {
                    if (ngx_processes[n].exited || ngx_processes[n].pid == -1 || ngx_processes[n].channel[0] == -1)
                    {
                        continue;
                    }

                    ngx_log_debug3(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                                   "pass close channel s:%i pid:%P to:%P",
                                   ch.slot, ch.pid, ngx_processes[n].pid);

                    /* TODO: NGX_AGAIN */

                    ngx_write_channel(ngx_processes[n].channel[0],
                                      &ch, sizeof(ngx_channel_t), cycle->log);
                }
            }

            if (ngx_processes[i].respawn && !ngx_processes[i].exiting && !ngx_terminate && !ngx_quit)
            {//子进程异常退出 重新调度起来
                if (ngx_spawn_process(cycle, ngx_processes[i].proc,
                                      ngx_processes[i].data,
                                      ngx_processes[i].name, i) == NGX_INVALID_PID)
                {
                    ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                                  "could not respawn %s",
                                  ngx_processes[i].name);
                    continue;
                }

                ch.command = NGX_CMD_OPEN_CHANNEL;
                ch.pid = ngx_processes[ngx_process_slot].pid;
                ch.slot = ngx_process_slot;
                ch.fd = ngx_processes[ngx_process_slot].channel[0];

                ngx_pass_open_channel(cycle, &ch);

                live = 1;

                continue;
            }

            if (ngx_processes[i].pid == ngx_new_binary)
            {

                ccf = (ngx_core_conf_t *)ngx_get_conf(cycle->conf_ctx,
                                                      ngx_core_module);

                if (ngx_rename_file((char *)ccf->oldpid.data,
                                    (char *)ccf->pid.data) == NGX_FILE_ERROR)
                {
                    ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                                  ngx_rename_file_n " %s back to %s failed "
                                                    "after the new binary process \"%s\" exited",
                                  ccf->oldpid.data, ccf->pid.data, ngx_argv[0]);
                }

                ngx_new_binary = 0;
                if (ngx_noaccepting)
                {
                    ngx_restart = 1;
                    ngx_noaccepting = 0;
                }
            }

            if (i == ngx_last_process - 1)
            {
                ngx_last_process--;
            }
            else
            {
                ngx_processes[i].pid = -1;
            }
        }
        else if (ngx_processes[i].exiting || !ngx_processes[i].detached)
        {
            live = 1;
        }
    }

    return live;
}

static void
ngx_master_process_exit(ngx_cycle_t *cycle)
{
    ngx_uint_t i;

    ngx_delete_pidfile(cycle);

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exit");

    for (i = 0; cycle->modules[i]; i++)
    {
        if (cycle->modules[i]->exit_master)
        {
            cycle->modules[i]->exit_master(cycle);
        }
    }

    ngx_close_listening_sockets(cycle);

    /*
     * Copy ngx_cycle->log related data to the special static exit cycle,
     * log, and log file structures enough to allow a signal handler to log.
     * The handler may be called when standard ngx_cycle->log allocated from
     * ngx_cycle->pool is already destroyed.
     */

    ngx_exit_log = *ngx_log_get_file_log(ngx_cycle->log);

    ngx_exit_log_file.fd = ngx_exit_log.file->fd;
    ngx_exit_log.file = &ngx_exit_log_file;
    ngx_exit_log.next = NULL;
    ngx_exit_log.writer = NULL;

    ngx_exit_cycle.log = &ngx_exit_log;
    ngx_exit_cycle.files = ngx_cycle->files;
    ngx_exit_cycle.files_n = ngx_cycle->files_n;
    ngx_cycle = &ngx_exit_cycle;

    ngx_destroy_pool(cycle->pool);

    exit(0);
}

/**
 * 子进程 进程main函数
 */
static void
ngx_worker_process_cycle(ngx_cycle_t *cycle, void *data)
{
    ngx_int_t worker = (intptr_t)data;

    ngx_process = NGX_PROCESS_WORKER;
    ngx_worker = worker;
    /**
     * 初始化worker进程
     * 将listening socket 和 channel添加到epoll事件驱动中
     */
    ngx_worker_process_init(cycle, worker);

    ngx_setproctitle("worker process"); /* 设置进程名称 */

    for (;;)
    {

        if (ngx_exiting)
        {
            if (ngx_event_no_timers_left() == NGX_OK)
            {//表示所有定时器任务均已经取消
                ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exiting");
                ngx_worker_process_exit(cycle);
            }
        }

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0, "worker cycle");
        /* 阻塞 等待事件或者定时器超时事件 */
        ngx_process_events_and_timers(cycle);

        if (ngx_terminate)
        {/* 处理Terminate事件 */
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exiting");
            ngx_worker_process_exit(cycle);
        }

        if (ngx_quit)
        {/* 处理Quit事件 */
            ngx_quit = 0;
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                          "gracefully shutting down");
            ngx_setproctitle("worker process is shutting down");

            if (!ngx_exiting)
            {
                ngx_exiting = 1;
                ngx_set_shutdown_timer(cycle);
                ngx_close_listening_sockets(cycle);
                ngx_close_idle_connections(cycle);
            }
        }

        if (ngx_reopen)
        {/* 处理Reopen事件 */
            ngx_reopen = 0;
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "reopening logs");
            ngx_reopen_files(cycle, -1);
        }
    }
}

/**
 * worker进程初始化
 * @param cycle 核心结构
 * @param worker 当前worker进程在ngx_processes数组索引
 */
static void
ngx_worker_process_init(ngx_cycle_t *cycle, ngx_int_t worker)
{
    sigset_t set;
    ngx_int_t n;
    ngx_time_t *tp;
    ngx_uint_t i;
    ngx_cpuset_t *cpu_affinity;
    struct rlimit rlmt;
    ngx_core_conf_t *ccf;
    ngx_listening_t *ls;

    if (ngx_set_environment(cycle, NULL) == NULL)
    {
        /* fatal */
        exit(2);
    }

    ccf = (ngx_core_conf_t *)ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    /* 设置资源使用门限阈值 */
    if (worker >= 0 && ccf->priority != 0)
    {
        if (setpriority(PRIO_PROCESS, 0, ccf->priority) == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "setpriority(%d) failed", ccf->priority);
        }
    }

    if (ccf->rlimit_nofile != NGX_CONF_UNSET)
    {
        rlmt.rlim_cur = (rlim_t)ccf->rlimit_nofile;
        rlmt.rlim_max = (rlim_t)ccf->rlimit_nofile;

        if (setrlimit(RLIMIT_NOFILE, &rlmt) == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "setrlimit(RLIMIT_NOFILE, %i) failed",
                          ccf->rlimit_nofile);
        }
    }

    if (ccf->rlimit_core != NGX_CONF_UNSET)
    {
        rlmt.rlim_cur = (rlim_t)ccf->rlimit_core;
        rlmt.rlim_max = (rlim_t)ccf->rlimit_core;

        if (setrlimit(RLIMIT_CORE, &rlmt) == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "setrlimit(RLIMIT_CORE, %O) failed",
                          ccf->rlimit_core);
        }
    }

    if (geteuid() == 0)
    {/* 设置进程用户、用户组信息 */
        if (setgid(ccf->group) == -1)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "setgid(%d) failed", ccf->group);
            /* fatal */
            exit(2);
        }

        if (initgroups(ccf->username, ccf->group) == -1)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "initgroups(%s, %d) failed",
                          ccf->username, ccf->group);
        }

        if (setuid(ccf->user) == -1)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "setuid(%d) failed", ccf->user);
            /* fatal */
            exit(2);
        }
    }

    if (worker >= 0)
    {//为了提升性能，Nginx采用cpu绑定进程方式
        cpu_affinity = ngx_get_cpu_affinity(worker);//进程绑定cpu

        if (cpu_affinity)
        {
            ngx_setaffinity(cpu_affinity, cycle->log);
        }
    }

#if (NGX_HAVE_PR_SET_DUMPABLE)

    /* allow coredump after setuid() in Linux 2.4.x */

    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "prctl(PR_SET_DUMPABLE) failed");
    }

#endif

    if (ccf->working_directory.len)
    {
        if (chdir((char *)ccf->working_directory.data) == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "chdir(\"%s\") failed", ccf->working_directory.data);
            /* fatal */
            exit(2);
        }
    }
    /* worker进程清空 信号屏蔽字 */
    sigemptyset(&set);

    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "sigprocmask() failed");
    }
    /* 初始化随机种子 */
    tp = ngx_timeofday();
    srandom(((unsigned)ngx_pid << 16) ^ tp->sec ^ tp->msec);

    /*
     * disable deleting previous events for the listening sockets because
     * in the worker processes there are no events at all at this point
     */
    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++)
    {
        ls[i].previous = NULL;
    }

    /**
     * ngx_event_core_module 定义init_proccess 该ngx_event_process_init方法将
     * listening socket注册到事件驱动中，用于接收连接事件
     * 重点内容
     */
    for (i = 0; cycle->modules[i]; i++)
    {
        if (cycle->modules[i]->init_process)
        {
            if (cycle->modules[i]->init_process(cycle) == NGX_ERROR)
            {
                /* fatal */
                exit(2);
            }
        }
    }
    /* 关闭除自己以外worker进程的channel通道 */
    for (n = 0; n < ngx_last_process; n++)
    {

        if (ngx_processes[n].pid == -1)
        {
            continue;
        }

        if (n == ngx_process_slot)
        {//不处理自己所在的线程
            continue;
        }

        if (ngx_processes[n].channel[1] == -1)
        {
            continue;
        }

        if (close(ngx_processes[n].channel[1]) == -1)
        {//关闭其他子进程socket
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "close() channel failed");
        }
    }
    /* 关闭父进程socket */
    if (close(ngx_processes[ngx_process_slot].channel[0]) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "close() channel failed");
    }

#if 0
    ngx_last_process = 0;
#endif
    /* 子进程将socketpair添加到epoll事件对象中 方便以后读取事件 */
    if (ngx_add_channel_event(cycle, ngx_channel, NGX_READ_EVENT,
                              ngx_channel_handler) == NGX_ERROR)
    {
        /* fatal */
        exit(2);
    }
}

static void
ngx_worker_process_exit(ngx_cycle_t *cycle)
{
    ngx_uint_t i;
    ngx_connection_t *c;

    for (i = 0; cycle->modules[i]; i++)
    {
        if (cycle->modules[i]->exit_process)
        {
            cycle->modules[i]->exit_process(cycle);
        }
    }

    if (ngx_exiting)
    {
        c = cycle->connections;
        for (i = 0; i < cycle->connection_n; i++)
        {
            if (c[i].fd != -1 && c[i].read && !c[i].read->accept && !c[i].read->channel && !c[i].read->resolver)
            {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "*%uA open socket #%d left in connection %ui",
                              c[i].number, c[i].fd, i);
                ngx_debug_quit = 1;
            }
        }

        if (ngx_debug_quit)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0, "aborting");
            ngx_debug_point();
        }
    }

    /*
     * Copy ngx_cycle->log related data to the special static exit cycle,
     * log, and log file structures enough to allow a signal handler to log.
     * The handler may be called when standard ngx_cycle->log allocated from
     * ngx_cycle->pool is already destroyed.
     */

    ngx_exit_log = *ngx_log_get_file_log(ngx_cycle->log);

    ngx_exit_log_file.fd = ngx_exit_log.file->fd;
    ngx_exit_log.file = &ngx_exit_log_file;
    ngx_exit_log.next = NULL;
    ngx_exit_log.writer = NULL;

    ngx_exit_cycle.log = &ngx_exit_log;
    ngx_exit_cycle.files = ngx_cycle->files;
    ngx_exit_cycle.files_n = ngx_cycle->files_n;
    ngx_cycle = &ngx_exit_cycle;

    ngx_destroy_pool(cycle->pool);

    ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0, "exit");

    exit(0);
}

static void
ngx_channel_handler(ngx_event_t *ev)
{
    ngx_int_t n;
    ngx_channel_t ch;
    ngx_connection_t *c;

    if (ev->timedout)
    {
        ev->timedout = 0;
        return;
    }

    c = ev->data;

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, ev->log, 0, "channel handler");

    for (;;)
    {

        n = ngx_read_channel(c->fd, &ch, sizeof(ngx_channel_t), ev->log);

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, ev->log, 0, "channel: %i", n);

        if (n == NGX_ERROR)
        {

            if (ngx_event_flags & NGX_USE_EPOLL_EVENT)
            {
                ngx_del_conn(c, 0);
            }

            ngx_close_connection(c);
            return;
        }

        if (ngx_event_flags & NGX_USE_EVENTPORT_EVENT)
        {
            if (ngx_add_event(ev, NGX_READ_EVENT, 0) == NGX_ERROR)
            {
                return;
            }
        }

        if (n == NGX_AGAIN)
        {
            return;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, ev->log, 0,
                       "channel command: %ui", ch.command);

        switch (ch.command)
        {

        case NGX_CMD_QUIT:
            ngx_quit = 1;
            break;

        case NGX_CMD_TERMINATE:
            ngx_terminate = 1;
            break;

        case NGX_CMD_REOPEN:
            ngx_reopen = 1;
            break;

        case NGX_CMD_OPEN_CHANNEL:

            ngx_log_debug3(NGX_LOG_DEBUG_CORE, ev->log, 0,
                           "get channel s:%i pid:%P fd:%d",
                           ch.slot, ch.pid, ch.fd);

            ngx_processes[ch.slot].pid = ch.pid;
            ngx_processes[ch.slot].channel[0] = ch.fd;
            break;

        case NGX_CMD_CLOSE_CHANNEL:

            ngx_log_debug4(NGX_LOG_DEBUG_CORE, ev->log, 0,
                           "close channel s:%i pid:%P our:%P fd:%d",
                           ch.slot, ch.pid, ngx_processes[ch.slot].pid,
                           ngx_processes[ch.slot].channel[0]);

            if (close(ngx_processes[ch.slot].channel[0]) == -1)
            {
                ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                              "close() channel failed");
            }

            ngx_processes[ch.slot].channel[0] = -1;
            break;
        }
    }
}

static void
ngx_cache_manager_process_cycle(ngx_cycle_t *cycle, void *data)
{
    ngx_cache_manager_ctx_t *ctx = data;

    void *ident[4];
    ngx_event_t ev;

    /*
     * Set correct process type since closing listening Unix domain socket
     * in a master process also removes the Unix domain socket file.
     */
    ngx_process = NGX_PROCESS_HELPER;

    ngx_close_listening_sockets(cycle);

    /* Set a moderate number of connections for a helper process. */
    cycle->connection_n = 512;

    ngx_worker_process_init(cycle, -1);

    ngx_memzero(&ev, sizeof(ngx_event_t));
    ev.handler = ctx->handler;
    ev.data = ident;
    ev.log = cycle->log;
    ident[3] = (void *)-1;

    ngx_use_accept_mutex = 0;

    ngx_setproctitle(ctx->name);

    ngx_add_timer(&ev, ctx->delay);

    for (;;)
    {

        if (ngx_terminate || ngx_quit)
        {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exiting");
            exit(0);
        }

        if (ngx_reopen)
        {
            ngx_reopen = 0;
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "reopening logs");
            ngx_reopen_files(cycle, -1);
        }

        ngx_process_events_and_timers(cycle);
    }
}

static void
ngx_cache_manager_process_handler(ngx_event_t *ev)
{
    ngx_uint_t i;
    ngx_msec_t next, n;
    ngx_path_t **path;

    next = 60 * 60 * 1000;

    path = ngx_cycle->paths.elts;
    for (i = 0; i < ngx_cycle->paths.nelts; i++)
    {

        if (path[i]->manager)
        {
            n = path[i]->manager(path[i]->data);

            next = (n <= next) ? n : next;

            ngx_time_update();
        }
    }

    if (next == 0)
    {
        next = 1;
    }

    ngx_add_timer(ev, next);
}

static void
ngx_cache_loader_process_handler(ngx_event_t *ev)
{
    ngx_uint_t i;
    ngx_path_t **path;
    ngx_cycle_t *cycle;

    cycle = (ngx_cycle_t *)ngx_cycle;

    path = cycle->paths.elts;
    for (i = 0; i < cycle->paths.nelts; i++)
    {

        if (ngx_terminate || ngx_quit)
        {
            break;
        }

        if (path[i]->loader)
        {
            path[i]->loader(path[i]->data);
            ngx_time_update();
        }
    }

    exit(0);
}
