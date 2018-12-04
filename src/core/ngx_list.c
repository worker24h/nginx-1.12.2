
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>

/**
 * 创建ngx_list_t
 * @param pool  内存池
 * @param n     数组大小
 * @param size  数组中每个元素大小
 * @return 返回可用ngx_list_t地址
 */
ngx_list_t *
ngx_list_create(ngx_pool_t *pool, ngx_uint_t n, size_t size)
{
    ngx_list_t  *list;

    list = ngx_palloc(pool, sizeof(ngx_list_t));//分配ngx_list_t头部信息
    if (list == NULL) {
        return NULL;
    }

    if (ngx_list_init(list, pool, n, size) != NGX_OK) {//初始化ngx_list_t
        return NULL;
    }

    return list;
}

/**
 * 添加元素
 * @param l 待操作的链表
 * @return  返回值可用地址,外部调用者进行赋值操作
 * @Describe 
 * 和以往使用链表方式不太一样
 * 入参:传入一个链表结构，然后计算该链表节点数目以及可写入地址,返回可用起始地址。
 * 具体赋值操作,在函数返回后，在调用的地方进行复制。
 */
void *
ngx_list_push(ngx_list_t *l)
{
    void             *elt;
    ngx_list_part_t  *last;

    last = l->last;
    //最后一个节点没有可用空间则进行重新分配
    if (last->nelts == l->nalloc) {//表示当前数组中可用空间为0，需要重新分配

        /* the last part is full, allocate a new list part */

        last = ngx_palloc(l->pool, sizeof(ngx_list_part_t));
        if (last == NULL) {
            return NULL;
        }

        last->elts = ngx_palloc(l->pool, l->nalloc * l->size);
        if (last->elts == NULL) {
            return NULL;
        }

        last->nelts = 0;
        last->next = NULL;

        l->last->next = last;
        l->last = last;
    }
    //偏移指针 返回可用空间
    elt = (char *) last->elts + l->size * last->nelts;
    last->nelts++;

    return elt;
}
