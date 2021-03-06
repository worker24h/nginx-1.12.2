
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#if (NGX_TEST_BUILD_EPOLL)

/* epoll declarations */

#define EPOLLIN 0x001
#define EPOLLPRI 0x002
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#define EPOLLRDNORM 0x040
#define EPOLLRDBAND 0x080
#define EPOLLWRNORM 0x100
#define EPOLLWRBAND 0x200
#define EPOLLMSG 0x400

#define EPOLLRDHUP 0x2000  /* 表示Tcp处于半关闭状态 */

#define EPOLLEXCLUSIVE 0x10000000
#define EPOLLONESHOT 0x40000000
#define EPOLLET 0x80000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef union epoll_data {
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event
{
    uint32_t events;
    epoll_data_t data;
};

int epoll_create(int size);

int epoll_create(int size)
{
    return -1;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    return -1;
}

int epoll_wait(int epfd, struct epoll_event *events, int nevents, int timeout);

int epoll_wait(int epfd, struct epoll_event *events, int nevents, int timeout)
{
    return -1;
}

#if (NGX_HAVE_EVENTFD)
#define SYS_eventfd 323
#endif

#if (NGX_HAVE_FILE_AIO)

#define SYS_io_setup 245
#define SYS_io_destroy 246
#define SYS_io_getevents 247

typedef u_int aio_context_t;

struct io_event
{
    uint64_t data; /* the data field from the iocb */
    uint64_t obj;  /* what iocb this event came from */
    int64_t res;   /* result code for this event */
    int64_t res2;  /* secondary result */
};

#endif
#endif /* NGX_TEST_BUILD_EPOLL */

typedef struct
{
    ngx_uint_t events;
    ngx_uint_t aio_requests;
} ngx_epoll_conf_t;

static ngx_int_t ngx_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer);
#if (NGX_HAVE_EVENTFD)
static ngx_int_t ngx_epoll_notify_init(ngx_log_t *log);
static void ngx_epoll_notify_handler(ngx_event_t *ev);
#endif
#if (NGX_HAVE_EPOLLRDHUP)
static void ngx_epoll_test_rdhup(ngx_cycle_t *cycle);
#endif
static void ngx_epoll_done(ngx_cycle_t *cycle);
static ngx_int_t ngx_epoll_add_event(ngx_event_t *ev, ngx_int_t event,
                                     ngx_uint_t flags);
static ngx_int_t ngx_epoll_del_event(ngx_event_t *ev, ngx_int_t event,
                                     ngx_uint_t flags);
static ngx_int_t ngx_epoll_add_connection(ngx_connection_t *c);
static ngx_int_t ngx_epoll_del_connection(ngx_connection_t *c,
                                          ngx_uint_t flags);
#if (NGX_HAVE_EVENTFD)
static ngx_int_t ngx_epoll_notify(ngx_event_handler_pt handler);
#endif
static ngx_int_t ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
                                          ngx_uint_t flags);

#if (NGX_HAVE_FILE_AIO)
static void ngx_epoll_eventfd_handler(ngx_event_t *ev);
#endif

static void *ngx_epoll_create_conf(ngx_cycle_t *cycle);
static char *ngx_epoll_init_conf(ngx_cycle_t *cycle, void *conf);

static int ep = -1; /* epoll 文件句柄 */
static struct epoll_event *event_list;
static ngx_uint_t nevents;

#if (NGX_HAVE_EVENTFD)
static int notify_fd = -1;
static ngx_event_t notify_event;
static ngx_connection_t notify_conn;
#endif

#if (NGX_HAVE_FILE_AIO)

int ngx_eventfd = -1;
aio_context_t ngx_aio_ctx = 0;

static ngx_event_t ngx_eventfd_event;
static ngx_connection_t ngx_eventfd_conn;

#endif

#if (NGX_HAVE_EPOLLRDHUP)
ngx_uint_t ngx_use_epoll_rdhup;
#endif

static ngx_str_t epoll_name = ngx_string("epoll");

static ngx_command_t ngx_epoll_commands[] = {

    {ngx_string("epoll_events"),
     NGX_EVENT_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot,
     0,
     offsetof(ngx_epoll_conf_t, events),
     NULL},

    {ngx_string("worker_aio_requests"),
     NGX_EVENT_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot,
     0,
     offsetof(ngx_epoll_conf_t, aio_requests),
     NULL},

    ngx_null_command};

static ngx_event_module_t ngx_epoll_module_ctx = {
    &epoll_name,
    ngx_epoll_create_conf, /* create configuration */
    ngx_epoll_init_conf,   /* init configuration */

    {
        ngx_epoll_add_event,      /* add an event */
        ngx_epoll_del_event,      /* delete an event */
        ngx_epoll_add_event,      /* enable an event */
        ngx_epoll_del_event,      /* disable an event */
        ngx_epoll_add_connection, /* add an connection */
        ngx_epoll_del_connection, /* delete an connection */
#if (NGX_HAVE_EVENTFD)
        ngx_epoll_notify, /* trigger a notify */
#else
        NULL, /* trigger a notify */
#endif
        ngx_epoll_process_events, /* process the events */
        ngx_epoll_init,           /* init the events */
        ngx_epoll_done,           /* done the events */
    }};

ngx_module_t ngx_epoll_module = {
    NGX_MODULE_V1,
    &ngx_epoll_module_ctx, /* module context */
    ngx_epoll_commands,    /* module directives */
    NGX_EVENT_MODULE,      /* module type */
    NULL,                  /* init master */
    NULL,                  /* init module */
    NULL,                  /* init process */
    NULL,                  /* init thread */
    NULL,                  /* exit thread */
    NULL,                  /* exit process */
    NULL,                  /* exit master */
    NGX_MODULE_V1_PADDING};

#if (NGX_HAVE_FILE_AIO)

/*
 * We call io_setup(), io_destroy() io_submit(), and io_getevents() directly
 * as syscalls instead of libaio usage, because the library header file
 * supports eventfd() since 0.3.107 version only.
 */

static int
io_setup(u_int nr_reqs, aio_context_t *ctx)
{
    return syscall(SYS_io_setup, nr_reqs, ctx);
}

static int
io_destroy(aio_context_t ctx)
{
    return syscall(SYS_io_destroy, ctx);
}

static int
io_getevents(aio_context_t ctx, long min_nr, long nr, struct io_event *events,
             struct timespec *tmo)
{
    return syscall(SYS_io_getevents, ctx, min_nr, nr, events, tmo);
}

static void
ngx_epoll_aio_init(ngx_cycle_t *cycle, ngx_epoll_conf_t *epcf)
{
    int n;
    struct epoll_event ee;

#if (NGX_HAVE_SYS_EVENTFD_H)
    ngx_eventfd = eventfd(0, 0);
#else
    ngx_eventfd = syscall(SYS_eventfd, 0);
#endif

    if (ngx_eventfd == -1)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "eventfd() failed");
        ngx_file_aio = 0;
        return;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "eventfd: %d", ngx_eventfd);

    n = 1;

    if (ioctl(ngx_eventfd, FIONBIO, &n) == -1)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "ioctl(eventfd, FIONBIO) failed");
        goto failed;
    }

    if (io_setup(epcf->aio_requests, &ngx_aio_ctx) == -1)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "io_setup() failed");
        goto failed;
    }

    ngx_eventfd_event.data = &ngx_eventfd_conn;
    ngx_eventfd_event.handler = ngx_epoll_eventfd_handler;
    ngx_eventfd_event.log = cycle->log;
    ngx_eventfd_event.active = 1;
    ngx_eventfd_conn.fd = ngx_eventfd;
    ngx_eventfd_conn.read = &ngx_eventfd_event;
    ngx_eventfd_conn.log = cycle->log;

    ee.events = EPOLLIN | EPOLLET;
    ee.data.ptr = &ngx_eventfd_conn;

    if (epoll_ctl(ep, EPOLL_CTL_ADD, ngx_eventfd, &ee) != -1)
    {
        return;
    }

    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                  "epoll_ctl(EPOLL_CTL_ADD, eventfd) failed");

    if (io_destroy(ngx_aio_ctx) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "io_destroy() failed");
    }

failed:

    if (close(ngx_eventfd) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "eventfd close() failed");
    }

    ngx_eventfd = -1;
    ngx_aio_ctx = 0;
    ngx_file_aio = 0;
}

#endif

/**
 * epoll模型初始化
 * @param cycle 核心结构体
 * @param timer 定时器超时时间
 */
static ngx_int_t
ngx_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    ngx_epoll_conf_t *epcf;
    /* 获取epoll模型下配置结构 */
    epcf = ngx_event_get_conf(cycle->conf_ctx, ngx_epoll_module);

    if (ep == -1)
    {   
        /**
         * 创建epoll对象 其中epoll_create参数没有意义
         */
        ep = epoll_create(cycle->connection_n / 2);

        if (ep == -1)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "epoll_create() failed");
            return NGX_ERROR;
        }

#if (NGX_HAVE_EVENTFD)
        /**
         * 使用eventfd实现 事件通知功能 主要用于多线程模式下  目前可忽略
         */
        if (ngx_epoll_notify_init(cycle->log) != NGX_OK)
        {
            ngx_epoll_module_ctx.actions.notify = NULL;
        }
#endif

#if (NGX_HAVE_FILE_AIO)
        ngx_epoll_aio_init(cycle, epcf);
#endif

#if (NGX_HAVE_EPOLLRDHUP)
        ngx_epoll_test_rdhup(cycle);
#endif
    }
    /**
     * 创建epoll_event数组 用于存储epoll返回事件
     * 数组大小为512
     */
    if (nevents < epcf->events)
    {
        if (event_list)
        {
            ngx_free(event_list);
        }

        event_list = ngx_alloc(sizeof(struct epoll_event) * epcf->events,
                               cycle->log);
        if (event_list == NULL)
        {
            return NGX_ERROR;
        }
    }

    nevents = epcf->events;//一次性 最大可保存 epoll事件数

    ngx_io = ngx_os_io; /* 回调函数赋值 主要收发回调函数 */

    ngx_event_actions = ngx_epoll_module_ctx.actions;

#if (NGX_HAVE_CLEAR_EVENT)
    ngx_event_flags = NGX_USE_CLEAR_EVENT  /* ET模式 */
#else
    ngx_event_flags = NGX_USE_LEVEL_EVENT
#endif
                      | NGX_USE_GREEDY_EVENT | NGX_USE_EPOLL_EVENT;

    return NGX_OK;
}

#if (NGX_HAVE_EVENTFD)

static ngx_int_t
ngx_epoll_notify_init(ngx_log_t *log)
{
    struct epoll_event ee;

#if (NGX_HAVE_SYS_EVENTFD_H)
    notify_fd = eventfd(0, 0);
#else
    notify_fd = syscall(SYS_eventfd, 0);
#endif

    if (notify_fd == -1)
    {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "eventfd() failed");
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
                   "notify eventfd: %d", notify_fd);

    notify_event.handler = ngx_epoll_notify_handler;
    notify_event.log = log;
    notify_event.active = 1;

    notify_conn.fd = notify_fd;
    notify_conn.read = &notify_event;
    notify_conn.log = log;

    ee.events = EPOLLIN | EPOLLET;
    ee.data.ptr = &notify_conn;

    if (epoll_ctl(ep, EPOLL_CTL_ADD, notify_fd, &ee) == -1)
    {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "epoll_ctl(EPOLL_CTL_ADD, eventfd) failed");

        if (close(notify_fd) == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                          "eventfd close() failed");
        }

        return NGX_ERROR;
    }

    return NGX_OK;
}

static void
ngx_epoll_notify_handler(ngx_event_t *ev)
{
    ssize_t n;
    uint64_t count;
    ngx_err_t err;
    ngx_event_handler_pt handler;

    if (++ev->index == NGX_MAX_UINT32_VALUE)
    {
        ev->index = 0;

        n = read(notify_fd, &count, sizeof(uint64_t));

        err = ngx_errno;

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                       "read() eventfd %d: %z count:%uL", notify_fd, n, count);

        if ((size_t)n != sizeof(uint64_t))
        {
            ngx_log_error(NGX_LOG_ALERT, ev->log, err,
                          "read() eventfd %d failed", notify_fd);
        }
    }

    handler = ev->data;
    handler(ev);
}

#endif

#if (NGX_HAVE_EPOLLRDHUP)

static void
ngx_epoll_test_rdhup(ngx_cycle_t *cycle)
{
    int s[2], events;
    struct epoll_event ee;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "socketpair() failed");
        return;
    }

    ee.events = EPOLLET | EPOLLIN | EPOLLRDHUP;

    if (epoll_ctl(ep, EPOLL_CTL_ADD, s[0], &ee) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "epoll_ctl() failed");
        goto failed;
    }

    if (close(s[1]) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "close() failed");
        s[1] = -1;
        goto failed;
    }

    s[1] = -1;

    events = epoll_wait(ep, &ee, 1, 5000);

    if (events == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "epoll_wait() failed");
        goto failed;
    }

    if (events)
    {
        ngx_use_epoll_rdhup = ee.events & EPOLLRDHUP;
    }
    else
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, NGX_ETIMEDOUT,
                      "epoll_wait() timed out");
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "testing the EPOLLRDHUP flag: %s",
                   ngx_use_epoll_rdhup ? "success" : "fail");

failed:

    if (s[1] != -1 && close(s[1]) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "close() failed");
    }

    if (close(s[0]) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "close() failed");
    }
}

#endif

static void
ngx_epoll_done(ngx_cycle_t *cycle)
{
    /* 关闭epoll对象 */
    if (close(ep) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "epoll close() failed");
    }

    ep = -1;

#if (NGX_HAVE_EVENTFD)
    /* 关闭eventfd对象 */
    if (close(notify_fd) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "eventfd close() failed");
    }

    notify_fd = -1;

#endif

#if (NGX_HAVE_FILE_AIO)

    if (ngx_eventfd != -1)
    {

        if (io_destroy(ngx_aio_ctx) == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "io_destroy() failed");
        }

        if (close(ngx_eventfd) == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "eventfd close() failed");
        }

        ngx_eventfd = -1;
    }

    ngx_aio_ctx = 0;

#endif

    ngx_free(event_list); //释放事件数组

    event_list = NULL;
    nevents = 0;
}

/**
 * 添加事件到事件驱动epoll中
 * @param ev 事件对象
 * @param event 事件类型 
 *        取值为 NGX_READ_EVENT NGX_WRITE_EVENT
 * @param flags
 */
static ngx_int_t
ngx_epoll_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    int op;
    uint32_t events, prev;
    ngx_event_t *e;
    ngx_connection_t *c;
    struct epoll_event ee;

    c = ev->data;

    events = (uint32_t)event; /* 对事件赋值 */

    if (event == NGX_READ_EVENT)
    {//注册读事件
        e = c->write;
        prev = EPOLLOUT;
#if (NGX_READ_EVENT != EPOLLIN | EPOLLRDHUP) //epoll模型下不会进入此分支
        events = EPOLLIN | EPOLLRDHUP;
#endif
    }
    else
    {//注册写事件
        e = c->read;
        prev = EPOLLIN | EPOLLRDHUP;
#if (NGX_WRITE_EVENT != EPOLLOUT) //epoll模型下不会进入此分支
        events = EPOLLOUT;
#endif
    }

    if (e->active)
    {//表示活跃事件 说明当前fd已经添加到epoll中 则进行修改操作
        op = EPOLL_CTL_MOD;
        events |= prev;
    }
    else
    {//非活跃事件 说明当前fd未添加到epoll中 则进行添加操作
        op = EPOLL_CTL_ADD;
    }

#if (NGX_HAVE_EPOLLEXCLUSIVE && NGX_HAVE_EPOLLRDHUP)
    if (flags & NGX_EXCLUSIVE_EVENT)
    {
        events &= ~EPOLLRDHUP;
    }
#endif
    /**
     * 1、设置event事件
     * 2、设置私有数据字段，当事件发生后epoll_wait会带回该字段。上层应用需要处理
     * 3、指针最后1bit始终为0，此处体现出nginx设计巧妙之处
     */
    ee.events = events | (uint32_t)flags;
    ee.data.ptr = (void *)((uintptr_t)c | ev->instance); 

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "epoll add event: fd:%d op:%d ev:%08XD",
                   c->fd, op, ee.events);

    if (epoll_ctl(ep, op, c->fd, &ee) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "epoll_ctl(%d, %d) failed", op, c->fd);
        return NGX_ERROR;
    }

    ev->active = 1;
#if 0
    ev->oneshot = (flags & NGX_ONESHOT_EVENT) ? 1 : 0;
#endif

    return NGX_OK;
}

/**
 * 从事件驱动epoll中删除事件
 * @param ev 待删除事件对象
 * @param event 事件类型 
 *        取值为 NGX_READ_EVENT NGX_WRITE_EVENT
 * @param flags
 */
static ngx_int_t
ngx_epoll_del_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    int op;
    uint32_t prev;
    ngx_event_t *e;
    ngx_connection_t *c;
    struct epoll_event ee;

    /*
     * when the file descriptor is closed, the epoll automatically deletes
     * it from its queue, so we do not need to delete explicitly the event
     * before the closing the file descriptor
     * 表示当前socket是关闭事件 那么直接将active设置为0即可
     */
    if (flags & NGX_CLOSE_EVENT)
    {
        ev->active = 0;
        return NGX_OK;
    }

    c = ev->data;

    if (event == NGX_READ_EVENT)
    {
        e = c->write;
        prev = EPOLLOUT;
    }
    else
    {
        e = c->read;
        prev = EPOLLIN | EPOLLRDHUP;
    }

    if (e->active)
    {
        op = EPOLL_CTL_MOD;
        ee.events = prev | (uint32_t)flags;
        ee.data.ptr = (void *)((uintptr_t)c | ev->instance);
    }
    else
    {
        op = EPOLL_CTL_DEL;
        ee.events = 0;
        ee.data.ptr = NULL;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "epoll del event: fd:%d op:%d ev:%08XD",
                   c->fd, op, ee.events);

    if (epoll_ctl(ep, op, c->fd, &ee) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "epoll_ctl(%d, %d) failed", op, c->fd);
        return NGX_ERROR;
    }

    ev->active = 0; //事件被移除 需要把active设置为0

    return NGX_OK;
}

static ngx_int_t
ngx_epoll_add_connection(ngx_connection_t *c)
{
    struct epoll_event ee;

    ee.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ee.data.ptr = (void *)((uintptr_t)c | c->read->instance);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "epoll add connection: fd:%d ev:%08XD", c->fd, ee.events);

    if (epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ee) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      "epoll_ctl(EPOLL_CTL_ADD, %d) failed", c->fd);
        return NGX_ERROR;
    }

    c->read->active = 1;
    c->write->active = 1;

    return NGX_OK;
}

static ngx_int_t
ngx_epoll_del_connection(ngx_connection_t *c, ngx_uint_t flags)
{
    int op;
    struct epoll_event ee;

    /*
     * when the file descriptor is closed the epoll automatically deletes
     * it from its queue so we do not need to delete explicitly the event
     * before the closing the file descriptor
     */

    if (flags & NGX_CLOSE_EVENT)
    {
        c->read->active = 0;
        c->write->active = 0;
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "epoll del connection: fd:%d", c->fd);

    op = EPOLL_CTL_DEL;
    ee.events = 0;
    ee.data.ptr = NULL;

    if (epoll_ctl(ep, op, c->fd, &ee) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      "epoll_ctl(%d, %d) failed", op, c->fd);
        return NGX_ERROR;
    }

    c->read->active = 0;
    c->write->active = 0;

    return NGX_OK;
}

#if (NGX_HAVE_EVENTFD)

static ngx_int_t
ngx_epoll_notify(ngx_event_handler_pt handler)
{
    static uint64_t inc = 1;

    notify_event.data = handler;

    if ((size_t)write(notify_fd, &inc, sizeof(uint64_t)) != sizeof(uint64_t))
    {
        ngx_log_error(NGX_LOG_ALERT, notify_event.log, ngx_errno,
                      "write() to eventfd %d failed", notify_fd);
        return NGX_ERROR;
    }

    return NGX_OK;
}

#endif

/**
 * 事件驱动
 * @param cycle 核心结构体
 * @param timer 等待时间
 * @param flags
 *        取值: NGX_POST_EVENTS、NGX_UPDATE_TIME 
 */
static ngx_int_t
ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    int events;
    uint32_t revents;
    ngx_int_t instance, i;
    ngx_uint_t level;
    ngx_err_t err;
    ngx_event_t *rev, *wev;
    ngx_queue_t *queue;
    ngx_connection_t *c;

    /* NGX_TIMER_INFINITE == INFTIM */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "epoll timer: %M", timer);

   /**
     * timer不是固定不变的，如果没有任何事件发生(空闲期)，
     * timer可能是NGX_TIMER_INFINITE 即表示永久阻塞
     */
    events = epoll_wait(ep, event_list, (int)nevents, timer);

    err = (events == -1) ? ngx_errno : 0;

    /**
     * Nginx两种时间策略: 
     *   1、如果nginx.conf文件中定义时间精度timer_resolution，则表示nginx的时间
     *      缓存精确到ngx_timer_resolution毫秒
     *   2、如果没有定义时间精度 则严格按照系统时间
     * ----------------------------------------------------------------
     * 条件说明:
     *     flags & NGX_UPDATE_TIME  -- 表示强制更新系统时间
     *     ngx_event_timer_alarm  当采用时间精度时，nginx会启动一个定时器，每次
     *                            超时，都会产生SIGALRM信号。具体参考ngx_event_process_init
     */
    if (flags & NGX_UPDATE_TIME || ngx_event_timer_alarm)
    {//只要是时间相关的事件 就立即更新时间缓存
        ngx_time_update(); //更新时间缓存
    }

    if (err)
    {
        if (err == NGX_EINTR)
        {

            if (ngx_event_timer_alarm)
            {//表示发生了SIGALRM信号中断 认为是正常场景
                ngx_event_timer_alarm = 0;
                return NGX_OK;
            }

            level = NGX_LOG_INFO;
        }
        else
        {//异常场景
            level = NGX_LOG_ALERT;
        }

        ngx_log_error(level, cycle->log, err, "epoll_wait() failed");
        return NGX_ERROR;
    }

    if (events == 0)
    {
        if (timer != NGX_TIMER_INFINITE)
        {//表示时间超时
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "epoll_wait() returned no events without timeout");
        return NGX_ERROR;
    }

    for (i = 0; i < events; i++)
    {
        c = event_list[i].data.ptr;
        /* 指针变量 最后一位始终为0  节省内存空间 */
        instance = (uintptr_t)c & 1;
        c = (ngx_connection_t *)((uintptr_t)c & (uintptr_t)~1);

        rev = c->read;

        if (c->fd == -1 || rev->instance != instance)
        {/**
          * 当fd=-1 或者instance不一致表示 当前事件是过期事件不需要处理
          * 《深入理解Nginx模块开发与架构解析》一书:318页 
          */
            /*
             * the stale event from a file descriptor
             * that was just closed in this iteration
             */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                           "epoll: stale event %p", c);
            continue;
        }

        revents = event_list[i].events;

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "epoll: fd:%d ev:%04XD d:%p",
                       c->fd, revents, event_list[i].data.ptr);

        if (revents & (EPOLLERR | EPOLLHUP))
        {
            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                           "epoll_wait() error on fd:%d ev:%04XD",
                           c->fd, revents);

            /*
             * if the error events were returned, add EPOLLIN and EPOLLOUT
             * to handle the events at least in one active handler
             */

            revents |= EPOLLIN | EPOLLOUT;
        }

#if 0
        if (revents & ~(EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP)) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "strange epoll_wait() events fd:%d ev:%04XD",
                          c->fd, revents);
        }
#endif

        if ((revents & EPOLLIN) && rev->active)
        {

#if (NGX_HAVE_EPOLLRDHUP)
            if (revents & EPOLLRDHUP)
            {
                rev->pending_eof = 1;
            }

            rev->available = 1;
#endif

            rev->ready = 1;

            if (flags & NGX_POST_EVENTS)
            { //需要延迟处理该事件
                queue = rev->accept ? &ngx_posted_accept_events
                                    : &ngx_posted_events;

                ngx_post_event(rev, queue); //将事件加入到事件队列中
            }
            else
            {//立即处理该事件
                rev->handler(rev); //ngx_http_keepalive_handler
            }
        }

        wev = c->write;

        if ((revents & EPOLLOUT) && wev->active)
        {

            if (c->fd == -1 || wev->instance != instance)
            {

                /*
                 * the stale event from a file descriptor
                 * that was just closed in this iteration
                 */

                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                               "epoll: stale event %p", c);
                continue;
            }

            wev->ready = 1;
#if (NGX_THREADS)
            wev->complete = 1;
#endif

            if (flags & NGX_POST_EVENTS)
            {//延迟处理该事件
                ngx_post_event(wev, &ngx_posted_events);
            }
            else
            {//立即处理该事件
                wev->handler(wev);
            }
        }
    }

    return NGX_OK;
}

#if (NGX_HAVE_FILE_AIO)

static void
ngx_epoll_eventfd_handler(ngx_event_t *ev)
{
    int n, events;
    long i;
    uint64_t ready;
    ngx_err_t err;
    ngx_event_t *e;
    ngx_event_aio_t *aio;
    struct io_event event[64];
    struct timespec ts;

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0, "eventfd handler");

    n = read(ngx_eventfd, &ready, 8);

    err = ngx_errno;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0, "eventfd: %d", n);

    if (n != 8)
    {
        if (n == -1)
        {
            if (err == NGX_EAGAIN)
            {
                return;
            }

            ngx_log_error(NGX_LOG_ALERT, ev->log, err, "read(eventfd) failed");
            return;
        }

        ngx_log_error(NGX_LOG_ALERT, ev->log, 0,
                      "read(eventfd) returned only %d bytes", n);
        return;
    }

    ts.tv_sec = 0;
    ts.tv_nsec = 0;

    while (ready)
    {

        events = io_getevents(ngx_aio_ctx, 1, 64, event, &ts);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                       "io_getevents: %d", events);

        if (events > 0)
        {
            ready -= events;

            for (i = 0; i < events; i++)
            {

                ngx_log_debug4(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                               "io_event: %XL %XL %L %L",
                               event[i].data, event[i].obj,
                               event[i].res, event[i].res2);

                e = (ngx_event_t *)(uintptr_t)event[i].data;

                e->complete = 1;
                e->active = 0;
                e->ready = 1;

                aio = e->data;
                aio->res = event[i].res;

                ngx_post_event(e, &ngx_posted_events);
            }

            continue;
        }

        if (events == 0)
        {
            return;
        }

        /* events == -1 */
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "io_getevents() failed");
        return;
    }
}

#endif

static void *
ngx_epoll_create_conf(ngx_cycle_t *cycle)
{
    ngx_epoll_conf_t *epcf;

    epcf = ngx_palloc(cycle->pool, sizeof(ngx_epoll_conf_t));
    if (epcf == NULL)
    {
        return NULL;
    }

    epcf->events = NGX_CONF_UNSET;
    epcf->aio_requests = NGX_CONF_UNSET;

    return epcf;
}

static char *
ngx_epoll_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_epoll_conf_t *epcf = conf;

    ngx_conf_init_uint_value(epcf->events, 512);
    ngx_conf_init_uint_value(epcf->aio_requests, 32);

    return NGX_CONF_OK;
}
