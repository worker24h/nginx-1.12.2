
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

#ifndef _NGX_CYCLE_H_INCLUDED_
#define _NGX_CYCLE_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>

#ifndef NGX_CYCLE_POOL_SIZE
#define NGX_CYCLE_POOL_SIZE NGX_DEFAULT_POOL_SIZE
#endif

#define NGX_DEBUG_POINTS_STOP 1
#define NGX_DEBUG_POINTS_ABORT 2

typedef struct ngx_shm_zone_s ngx_shm_zone_t;

typedef ngx_int_t (*ngx_shm_zone_init_pt)(ngx_shm_zone_t *zone, void *data);

struct ngx_shm_zone_s
{
    void *data;
    ngx_shm_t shm;
    ngx_shm_zone_init_pt init;
    void *tag;
    ngx_uint_t noreuse; /* unsigned  noreuse:1; */
};

struct ngx_cycle_s
{
     /**
      * ָ��һ��ָ�����飬������Ԫ����ָ����һ��ָ�����飬���ʱ4��ָ��.
      * ��һ�������±꣬��ngx_module_t.index
      * �ڶ��������±꣬��ngx_module_t.ctx_index
      * �������ýṹ�壬ÿ��ģ�鶼һ������ģ��ṹ��(�Զ���). 
      * ��create_conf�ص��������������ýṹ��
      * ע: conf_ctx��һ�����飬��С��ngx_modulesһ�� ��ʼ���ں���ngx_init_cycle
      */
    void ****conf_ctx;
    ngx_pool_t *pool; /* ���̼��ڴ�� */

    ngx_log_t *log;
    ngx_log_t new_log;

    ngx_uint_t log_use_stderr; /* unsigned  log_use_stderr:1; */

    ngx_connection_t **files;
    ngx_connection_t *free_connections; /* �������� */
    ngx_uint_t free_connection_n; /* ���������� */

    ngx_module_t **modules; /* ʵ��ָ��ngx_modules.c��ngx_modules */
    ngx_uint_t modules_n;
    ngx_uint_t modules_used; /* unsigned  modules_used:1; */

    ngx_queue_t reusable_connections_queue; /* ˫������ ����connections ���ظ�ʹ�� */
    ngx_uint_t reusable_connections_n;

    ngx_array_t listening; /* ��̬���� ����socket */
    ngx_array_t paths;

    ngx_array_t config_dump;
    ngx_rbtree_t config_dump_rbtree;
    ngx_rbtree_node_t config_dump_sentinel;

    ngx_list_t open_files;
    ngx_list_t shared_memory;

    ngx_uint_t connection_n; /* ��ǰ��Ծ������ */
    ngx_uint_t files_n;

    /* ���߶�Ӧ��ϵ�� ���������±��Ӧ */
    ngx_connection_t *connections; /* ���ӳ� ���� Ĭ��1024 */
    ngx_event_t *read_events; /* ���� ÿһ���������ٶ�Ӧһ�����¼� Ĭ��1024 */
    ngx_event_t *write_events;/* ���� ÿһ���������ٶ�Ӧһ��д�¼� Ĭ��1024 */

    ngx_cycle_t *old_cycle;

    ngx_str_t conf_file; /* Ĭ��/usr/local/nginx/conf/nginx.conf */
    ngx_str_t conf_param;
    ngx_str_t conf_prefix; /* Ĭ��/usr/local/nginx/conf/ */
    ngx_str_t prefix; /* /usr/local/nginx/ */
    ngx_str_t lock_file;
    ngx_str_t hostname;
};

typedef struct
{
    ngx_flag_t daemon;
    ngx_flag_t master;

    ngx_msec_t timer_resolution;
    ngx_msec_t shutdown_timeout;

    ngx_int_t worker_processes;
    ngx_int_t debug_points;

    ngx_int_t rlimit_nofile;
    off_t rlimit_core;

    int priority;

    ngx_uint_t cpu_affinity_auto;
    ngx_uint_t cpu_affinity_n;
    ngx_cpuset_t *cpu_affinity;

    char *username;
    ngx_uid_t user;
    ngx_gid_t group;

    ngx_str_t working_directory;
    ngx_str_t lock_file;

    ngx_str_t pid;
    ngx_str_t oldpid;

    ngx_array_t env;
    char **environment;
} ngx_core_conf_t;

#define ngx_is_init_cycle(cycle) (cycle->conf_ctx == NULL)

ngx_cycle_t *ngx_init_cycle(ngx_cycle_t *old_cycle);
ngx_int_t ngx_create_pidfile(ngx_str_t *name, ngx_log_t *log);
void ngx_delete_pidfile(ngx_cycle_t *cycle);
ngx_int_t ngx_signal_process(ngx_cycle_t *cycle, char *sig);
void ngx_reopen_files(ngx_cycle_t *cycle, ngx_uid_t user);
char **ngx_set_environment(ngx_cycle_t *cycle, ngx_uint_t *last);
ngx_pid_t ngx_exec_new_binary(ngx_cycle_t *cycle, char *const *argv);
ngx_cpuset_t *ngx_get_cpu_affinity(ngx_uint_t n);
ngx_shm_zone_t *ngx_shared_memory_add(ngx_conf_t *cf, ngx_str_t *name,
                                      size_t size, void *tag);
void ngx_set_shutdown_timer(ngx_cycle_t *cycle);

extern volatile ngx_cycle_t *ngx_cycle;
extern ngx_array_t ngx_old_cycles;
extern ngx_module_t ngx_core_module;
extern ngx_uint_t ngx_test_config;
extern ngx_uint_t ngx_dump_config;
extern ngx_uint_t ngx_quiet_mode;

#endif /* _NGX_CYCLE_H_INCLUDED_ */