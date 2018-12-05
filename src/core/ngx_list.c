
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>

/**
 * ����ngx_list_t
 * @param pool  �ڴ��
 * @param n     �����С
 * @param size  ������ÿ��Ԫ�ش�С
 * @return ���ؿ���ngx_list_t��ַ
 */
ngx_list_t *
ngx_list_create(ngx_pool_t *pool, ngx_uint_t n, size_t size)
{
    ngx_list_t  *list;

    list = ngx_palloc(pool, sizeof(ngx_list_t));//����ngx_list_tͷ����Ϣ
    if (list == NULL) {
        return NULL;
    }

    if (ngx_list_init(list, pool, n, size) != NGX_OK) {//��ʼ��ngx_list_t
        return NULL;
    }

    return list;
}

/**
 * ���Ԫ��
 * @param l ������������
 * @return  ����ֵ���õ�ַ,�ⲿ�����߽��и�ֵ����
 * @Describe 
 * ������ʹ������ʽ��̫һ��
 * ���:����һ������ṹ��Ȼ����������ڵ���Ŀ�Լ���д���ַ,���ؿ�����ʼ��ַ��
 * ���帳ֵ����,�ں������غ��ڵ��õĵط����и��ơ�
 */
void *
ngx_list_push(ngx_list_t *l)
{
    void             *elt;
    ngx_list_part_t  *last;

    last = l->last;
    //���һ���ڵ�û�п��ÿռ���������·���
    if (last->nelts == l->nalloc) {//��ʾ��ǰ�����п��ÿռ�Ϊ0����Ҫ���·���

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
    //ƫ��ָ�� ���ؿ��ÿռ�
    elt = (char *) last->elts + l->size * last->nelts;
    last->nelts++;

    return elt;
}
