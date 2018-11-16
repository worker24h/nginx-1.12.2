
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

ngx_queue_t ngx_posted_accept_events; //队列头指针 只针对accept事件 优先级高
ngx_queue_t ngx_posted_events;        //队列头指针 针对除accept事件以外所有事件 优先级低

/**
 * 轮询队列，逐一调用回调方法handler
 */
void ngx_event_process_posted(ngx_cycle_t *cycle, ngx_queue_t *posted)
{
    ngx_queue_t *q;
    ngx_event_t *ev;

    while (!ngx_queue_empty(posted))
    {

        q = ngx_queue_head(posted);
        ev = ngx_queue_data(q, ngx_event_t, queue);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "posted event %p", ev);

        ngx_delete_posted_event(ev);//从队列中移除节点

        ev->handler(ev); //调用回调
    }
}
