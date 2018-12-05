
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>

/**
 * ����һ��buf������Ϊtemporary
 * @param pool �ڴ��
 * @param size ��ǰbuf��С
 */
ngx_buf_t *
ngx_create_temp_buf(ngx_pool_t *pool, size_t size)
{
    ngx_buf_t *b;

    b = ngx_calloc_buf(pool);//������һ��bufͷ��
    if (b == NULL) {
        return NULL;
    }

    b->start = ngx_palloc(pool, size);//buf������
    if (b->start == NULL) {//����ʧ��
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
    //���ø���ָ��
    b->pos = b->start;
    b->last = b->start;
    b->end = b->last + size;
    b->temporary = 1;//���Ϊ��ʱ�ڴ�

    return b;
}

/**
 * ����һ��chain�ڵ�
 * @param pool �ڴ��
 */
ngx_chain_t *
ngx_alloc_chain_link(ngx_pool_t *pool)
{
    ngx_chain_t  *cl;

    cl = pool->chain;
    /* NginxΪ������Ч�ʣ�����Ѿ�ʹ�ù�ngx_chain_t���浽ngx_pool_t���Ա��´�ʹ�� */
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
 * ����һ��buf����
 * @param pool �ڴ��
 * @param bufs buf����
 */
ngx_chain_t *
ngx_create_chain_of_bufs(ngx_pool_t *pool, ngx_bufs_t *bufs)
{
    u_char       *p;
    ngx_int_t     i;
    ngx_buf_t    *b;
    ngx_chain_t  *chain, *cl, **ll;

    p = ngx_palloc(pool, bufs->num * bufs->size);//����buf������
    if (p == NULL) {
        return NULL;
    }

    ll = &chain;

    for (i = 0; i < bufs->num; i++) {

        b = ngx_calloc_buf(pool);//Ϊnum��buf������bufͷ
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
        p += bufs->size;//����pָ��
        b->end = p;

        cl = ngx_alloc_chain_link(pool);//����ngx_chain_t ��֯����
        if (cl == NULL) {
            return NULL;
        }

        cl->buf = b;
        *ll = cl;//��������
        ll = &cl->next;
    }

    *ll = NULL;

    return chain;
}

/**
 * ����buf����
 * @param pool �ڴ��
 * @param chain �������
 * @param in    �������
 */
ngx_int_t
ngx_chain_add_copy(ngx_pool_t *pool, ngx_chain_t **chain, ngx_chain_t *in)
{
    ngx_chain_t  *cl, **ll;

    ll = chain;
    //�ҵ�����chain���һ���ڵ�
    for (cl = *chain; cl; cl = cl->next) {
        ll = &cl->next;
    }

    //ѭ������in
    while (in) {
        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = in->buf;//ֻ��ָ��ָ�� ������ʵ�����ݿ���
        *ll = cl;
        ll = &cl->next;
        in = in->next;
    }

    *ll = NULL;

    return NGX_OK;
}

/**
 * ��chain���л�ȡһ������buf
 * @param pool �ڴ��
 * @param free ������chain��
 */
ngx_chain_t *
ngx_chain_get_free_buf(ngx_pool_t *p, ngx_chain_t **free)
{
    ngx_chain_t  *cl;

    if (*free) {//����������� ���ʾ���� ����ֱ��ȡһ���ڵ㼴�ɷ���
        cl = *free;
        *free = cl->next;
        cl->next = NULL;
        return cl;
    }
    //free����Ϊ�� ����Ҫ���ڴ���з���һ���µĽڵ�
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
 * ����chain���� �ͷ��ڴ� ���ű���
 * @param p �ڴ��
 * @param free ������
 * @param busy ����ʹ����
 * @param out  ��out�зַ��� free����busy��
 * @param tag  ��־ �ַ�ԭ��
 */
void
ngx_chain_update_chains(ngx_pool_t *p, ngx_chain_t **free, ngx_chain_t **busy,
    ngx_chain_t **out, ngx_buf_tag_t tag)
{
    ngx_chain_t  *cl;

    /* ��out�����нڵ�ҵ�busy�� */
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

        if (ngx_buf_size(cl->buf) != 0) {//��ʾ��ǰbuf����δ���������� ֱ���˳�
            break;
        }

        /* ���tag��һ�� ��chain�ҵ�pool�� */
        if (cl->buf->tag != tag) {
            *busy = cl->next;
            ngx_free_chain(p, cl);
            continue;
        }
        /* �޸�ָ�벢�ҵ�free���� */
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