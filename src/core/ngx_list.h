
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_LIST_H_INCLUDED_
#define _NGX_LIST_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


typedef struct ngx_list_part_s  ngx_list_part_t;
/**
 * 链表数据节点
 */
struct ngx_list_part_s {
    void             *elts;//存储空间 相当于数组首地址
    ngx_uint_t        nelts;//已经使用节点数目 当nelts==nalloc表示已经存满
    ngx_list_part_t  *next;//指向下一个节点
};

/**
 * 链表
 */
typedef struct {
    ngx_list_part_t  *last; //保存最后一个节点 定义成指针方便修改指向
    ngx_list_part_t   part; //链表首节点
    size_t            size; //每个数组元素的大小 ngx_list_part_s.elts数组中一个元素大小
    ngx_uint_t        nalloc; //分配了n个节点，相当于n元素的数组
    ngx_pool_t       *pool;
} ngx_list_t;


ngx_list_t *ngx_list_create(ngx_pool_t *pool, ngx_uint_t n, size_t size);

/**
 * 初始化ngx_list_t
 * @param list  待初始化的ngx_list_t
 * @param poll  内存池
 * @param n     每个数组包含的元素个数
 * @param size  数组元素大小
 */
static ngx_inline ngx_int_t
ngx_list_init(ngx_list_t *list, ngx_pool_t *pool, ngx_uint_t n, size_t size)
{
    list->part.elts = ngx_palloc(pool, n * size);//分配数组空间
    if (list->part.elts == NULL) {
        return NGX_ERROR;
    }
    //设置各类指针
    list->part.nelts = 0;
    list->part.next = NULL;
    list->last = &list->part;
    list->size = size;
    list->nalloc = n;
    list->pool = pool;

    return NGX_OK;
}


/*
 *
 *  the iteration through the list:
 *
 *  part = &list.part;
 *  data = part->elts;
 *
 *  for (i = 0 ;; i++) {
 *
 *      if (i >= part->nelts) {
 *          if (part->next == NULL) {
 *              break;
 *          }
 *
 *          part = part->next;
 *          data = part->elts;
 *          i = 0;
 *      }
 *
 *      ...  data[i] ...
 *
 *  }
 */


void *ngx_list_push(ngx_list_t *list);


#endif /* _NGX_LIST_H_INCLUDED_ */
