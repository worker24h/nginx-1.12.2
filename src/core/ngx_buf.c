
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>

/**
 * 申请一个buf，类型为temporary
 * @param pool 内存池
 * @param size 当前buf大小
 */
ngx_buf_t *
ngx_create_temp_buf(ngx_pool_t *pool, size_t size)
{
    ngx_buf_t *b;

    b = ngx_calloc_buf(pool);//先申请一个buf头部
    if (b == NULL) {
        return NULL;
    }

    b->start = ngx_palloc(pool, size);//buf内容体
    if (b->start == NULL) {//申请失败
        return NULL;
    }

    /*
     * set by ngx_calloc_buf():
     *
     *     b->file_pos = 0;
     *     b->file_last = 0;
     *     b->file = NULL;
     *     b->shadow = NULL;
     *     b->tag = 0;
     *     and flags
     */
    //设置各类指针
    b->pos = b->start;
    b->last = b->start;
    b->end = b->last + size;
    b->temporary = 1;//标记为临时内存

    return b;
}

/**
 * 分配一个chain节点
 * @param pool 内存池
 */
ngx_chain_t *
ngx_alloc_chain_link(ngx_pool_t *pool)
{
    ngx_chain_t  *cl;

    cl = pool->chain;
    /* Nginx为了提升效率，会把已经使用过ngx_chain_t保存到ngx_pool_t中以便下次使用 */
    if (cl) {
        pool->chain = cl->next;
        return cl;
    }

    cl = ngx_palloc(pool, sizeof(ngx_chain_t));
    if (cl == NULL) {
        return NULL;
    }

    return cl;
}

/**
 * 分配一个buf链表
 * @param pool 内存池
 * @param bufs buf集合
 */
ngx_chain_t *
ngx_create_chain_of_bufs(ngx_pool_t *pool, ngx_bufs_t *bufs)
{
    u_char       *p;
    ngx_int_t     i;
    ngx_buf_t    *b;
    ngx_chain_t  *chain, *cl, **ll;

    p = ngx_palloc(pool, bufs->num * bufs->size);//申请buf内容体
    if (p == NULL) {
        return NULL;
    }

    ll = &chain;

    for (i = 0; i < bufs->num; i++) {

        b = ngx_calloc_buf(pool);//为num个buf，申请buf头
        if (b == NULL) {
            return NULL;
        }

        /*
         * set by ngx_calloc_buf():
         *
         *     b->file_pos = 0;
         *     b->file_last = 0;
         *     b->file = NULL;
         *     b->shadow = NULL;
         *     b->tag = 0;
         *     and flags
         *
         */

        b->pos = p;
        b->last = p;
        b->temporary = 1;

        b->start = p;
        p += bufs->size;//调整p指针
        b->end = p;

        cl = ngx_alloc_chain_link(pool);//申请ngx_chain_t 组织链表
        if (cl == NULL) {
            return NULL;
        }

        cl->buf = b;
        *ll = cl;//插入链表
        ll = &cl->next;
    }

    *ll = NULL;

    return chain;
}

/**
 * 合并buf链表 将in链表合并到chain中
 * @param pool 内存池
 * @param chain 输出参数
 * @param in    输入参数
 */
ngx_int_t
ngx_chain_add_copy(ngx_pool_t *pool, ngx_chain_t **chain, ngx_chain_t *in)
{
    ngx_chain_t  *cl, **ll;

    ll = chain;
    //找到链表chain最后一个节点
    for (cl = *chain; cl; cl = cl->next) {
        ll = &cl->next;
    }

    //循环遍历in
    while (in) {
        cl = ngx_alloc_chain_link(pool);//分配链头
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = in->buf;//只做指针指向 不进行实际内容拷贝
        *ll = cl;
        ll = &cl->next;
        in = in->next;
    }

    *ll = NULL;

    return NGX_OK;
}

/**
 * 从chain链中获取一个空闲buf
 * @param pool 内存池
 * @param free 待查找chain链
 */
ngx_chain_t *
ngx_chain_get_free_buf(ngx_pool_t *p, ngx_chain_t **free)
{
    ngx_chain_t  *cl;

    if (*free) {//如果链表不空 则表示空闲 所以直接取一个节点即可返回
        cl = *free;
        *free = cl->next;
        cl->next = NULL;
        return cl;
    }
    //free链表为空 则需要从内存池中分配一个新的节点
    cl = ngx_alloc_chain_link(p);
    if (cl == NULL) {
        return NULL;
    }

    cl->buf = ngx_calloc_buf(p);
    if (cl->buf == NULL) {
        return NULL;
    }

    cl->next = NULL;

    return cl;
}

/**
 * 更新chain链表 释放内存 将busy中空闲节点回到free链表中或者内存池中
 * @param p 内存池
 * @param free 空闲链
 * @param busy 正在使用链
 * @param out  将out中分发到 free或者busy中
 * @param tag  标志 分发原则 一般是函数指针
 */
void
ngx_chain_update_chains(ngx_pool_t *p, ngx_chain_t **free, ngx_chain_t **busy,
    ngx_chain_t **out, ngx_buf_tag_t tag)
{
    ngx_chain_t  *cl;

    /* 将out中所有节点挂到busy中 */
    if (*out) {
        if (*busy == NULL) {
            *busy = *out;

        } else {
            for (cl = *busy; cl->next; cl = cl->next) { /* void */ }

            cl->next = *out;
        }

        *out = NULL;
    }

    while (*busy) {
        cl = *busy;

        if (ngx_buf_size(cl->buf) != 0) {//表示当前buf有尚未处理的数据 直接退出
            break;
        }

        /**
         * 如果tag不同(可以理解成该buf是由那个模块创建)，则回到内存池中
         */
        if (cl->buf->tag != tag) {
            *busy = cl->next; //处理下一个节点
            ngx_free_chain(p, cl); //会受到内存池总
            continue;
        }
        
        /* tag相同 表示相同模块申请的buf 因此回收到free链表中 */
        cl->buf->pos = cl->buf->start;
        cl->buf->last = cl->buf->start;

        *busy = cl->next;
        cl->next = *free;
        *free = cl;
    }
}


off_t
ngx_chain_coalesce_file(ngx_chain_t **in, off_t limit)
{
    off_t         total, size, aligned, fprev;
    ngx_fd_t      fd;
    ngx_chain_t  *cl;

    total = 0;

    cl = *in;
    fd = cl->buf->file->fd;

    do {
        size = cl->buf->file_last - cl->buf->file_pos;

        if (size > limit - total) {
            size = limit - total;

            aligned = (cl->buf->file_pos + size + ngx_pagesize - 1)
                       & ~((off_t) ngx_pagesize - 1);

            if (aligned <= cl->buf->file_last) {
                size = aligned - cl->buf->file_pos;
            }

            total += size;
            break;
        }

        total += size;
        fprev = cl->buf->file_pos + size;
        cl = cl->next;

    } while (cl
             && cl->buf->in_file
             && total < limit
             && fd == cl->buf->file->fd
             && fprev == cl->buf->file_pos);

    *in = cl;

    return total;
}


ngx_chain_t *
ngx_chain_update_sent(ngx_chain_t *in, off_t sent)
{
    off_t  size;

    for ( /* void */ ; in; in = in->next) {

        if (ngx_buf_special(in->buf)) {
            continue;
        }

        if (sent == 0) {
            break;
        }

        size = ngx_buf_size(in->buf);

        if (sent >= size) {
            sent -= size;

            if (ngx_buf_in_memory(in->buf)) {
                in->buf->pos = in->buf->last;
            }

            if (in->buf->in_file) {
                in->buf->file_pos = in->buf->file_last;
            }

            continue;
        }

        if (ngx_buf_in_memory(in->buf)) {
            in->buf->pos += (size_t) sent;
        }

        if (in->buf->in_file) {
            in->buf->file_pos += sent;
        }

        break;
    }

    return in;
}
