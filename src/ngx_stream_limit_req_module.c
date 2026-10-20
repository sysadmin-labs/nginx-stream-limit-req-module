/*
 * Copyright (c) 2026, Jochen Neumeister <joneum@FreeBSD.org>
 * All rights reserved.
 *
 * The rate arithmetic follows ngx_http_limit_req_module, Copyright (C)
 * Igor Sysoev, Copyright (C) Nginx, Inc., so that a rate configured here
 * means what it means in an http block.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
 * ANY DAMAGES WHATSOEVER ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>


#define NGX_STREAM_LIMIT_REQ_PASSED            1
#define NGX_STREAM_LIMIT_REQ_DELAYED           2
#define NGX_STREAM_LIMIT_REQ_REJECTED          3
#define NGX_STREAM_LIMIT_REQ_DELAYED_DRY_RUN   4
#define NGX_STREAM_LIMIT_REQ_REJECTED_DRY_RUN  5


typedef struct {
    u_char                       color;
    u_char                       dummy;
    u_short                      len;
    ngx_queue_t                  queue;
    ngx_msec_t                   last;
    /* integer value, 1000 times the real excess, as in the http module */
    ngx_uint_t                   excess;
    ngx_uint_t                   count;
    u_char                       data[1];
} ngx_stream_limit_req_node_t;


typedef struct {
    ngx_rbtree_t                 rbtree;
    ngx_rbtree_node_t            sentinel;
    ngx_queue_t                  queue;
} ngx_stream_limit_req_shctx_t;


typedef struct {
    ngx_stream_limit_req_shctx_t  *sh;
    ngx_slab_pool_t               *shpool;
    /* rate in requests per second, 1000 times, so 5r/m is representable */
    ngx_uint_t                     rate;
    ngx_stream_complex_value_t     key;
    ngx_stream_limit_req_node_t   *node;
} ngx_stream_limit_req_ctx_t;


typedef struct {
    ngx_shm_zone_t              *shm_zone;
    ngx_uint_t                   burst;
    ngx_uint_t                   delay;
} ngx_stream_limit_req_limit_t;


typedef struct {
    ngx_array_t                  limits;
    ngx_uint_t                   limit_log_level;
    ngx_uint_t                   delay_log_level;
    ngx_flag_t                   dry_run;
} ngx_stream_limit_req_srv_conf_t;


typedef struct {
    ngx_uint_t                   status;      /* what happened to it */
    unsigned                     handled:1;   /* the accounting is done */
    unsigned                     delayed:1;   /* waiting on the timer below */
    ngx_event_t                  event;       /* the delay timer */
} ngx_stream_limit_req_session_ctx_t;


static ngx_str_t  ngx_stream_limit_req_status[] = {
    ngx_string("PASSED"),
    ngx_string("DELAYED"),
    ngx_string("REJECTED"),
    ngx_string("DELAYED_DRY_RUN"),
    ngx_string("REJECTED_DRY_RUN")
};


static ngx_str_t  ngx_stream_limit_req_status_name =
    ngx_string("limit_req_status");


static void ngx_stream_limit_req_delay(ngx_event_t *ev);
static ngx_int_t ngx_stream_limit_req_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_stream_limit_req_status_variable(
    ngx_stream_session_t *s, ngx_stream_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_stream_limit_req_lookup(
    ngx_stream_limit_req_limit_t *limit, ngx_uint_t hash, ngx_str_t *key,
    ngx_uint_t *ep, ngx_uint_t account);
static ngx_msec_t ngx_stream_limit_req_account(
    ngx_stream_limit_req_limit_t *limits, ngx_uint_t n, ngx_uint_t *ep,
    ngx_stream_limit_req_limit_t **limit);
static void ngx_stream_limit_req_expire(ngx_stream_limit_req_ctx_t *ctx,
    ngx_uint_t n);
static void ngx_stream_limit_req_unlock(ngx_stream_limit_req_limit_t *limits,
    ngx_uint_t n);

static void ngx_stream_limit_req_rbtree_insert_value(
    ngx_rbtree_node_t *temp, ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel);
static ngx_int_t ngx_stream_limit_req_init_zone(ngx_shm_zone_t *shm_zone,
    void *data);
static void *ngx_stream_limit_req_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_limit_req_merge_srv_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_stream_limit_req_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_stream_limit_req(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_stream_limit_req_init(ngx_conf_t *cf);


static ngx_conf_enum_t  ngx_stream_limit_req_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};


static ngx_command_t  ngx_stream_limit_req_commands[] = {

    { ngx_string("limit_req_zone"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE3,
      ngx_stream_limit_req_zone,
      0,
      0,
      NULL },

    { ngx_string("limit_req"),
      NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_TAKE123,
      ngx_stream_limit_req,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_req_log_level"),
      NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_limit_req_srv_conf_t, limit_log_level),
      &ngx_stream_limit_req_log_levels },

    { ngx_string("limit_req_dry_run"),
      NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_limit_req_srv_conf_t, dry_run),
      NULL },

      ngx_null_command
};


static ngx_stream_module_t  ngx_stream_limit_req_module_ctx = {
    ngx_stream_limit_req_add_variables,         /* preconfiguration */
    ngx_stream_limit_req_init,                  /* postconfiguration */

    NULL,                                       /* create main configuration */
    NULL,                                       /* init main configuration */

    ngx_stream_limit_req_create_srv_conf,       /* create server configuration */
    ngx_stream_limit_req_merge_srv_conf         /* merge server configuration */
};


ngx_module_t  ngx_stream_limit_req_module = {
    NGX_MODULE_V1,
    &ngx_stream_limit_req_module_ctx,           /* module context */
    ngx_stream_limit_req_commands,              /* module directives */
    NGX_STREAM_MODULE,                          /* module type */
    NULL,                                       /* init master */
    NULL,                                       /* init module */
    NULL,                                       /* init process */
    NULL,                                       /* init thread */
    NULL,                                       /* exit thread */
    NULL,                                       /* exit process */
    NULL,                                       /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_stream_limit_req_add_variables(ngx_conf_t *cf)
{
    ngx_stream_variable_t  *var;

    var = ngx_stream_add_variable(cf, &ngx_stream_limit_req_status_name,
                                  NGX_STREAM_VAR_NOCACHEABLE);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_stream_limit_req_status_variable;

    return NGX_OK;
}


static ngx_int_t
ngx_stream_limit_req_status_variable(ngx_stream_session_t *s,
    ngx_stream_variable_value_t *v, uintptr_t data)
{
    ngx_stream_limit_req_session_ctx_t  *sctx;

    sctx = ngx_stream_get_module_ctx(s, ngx_stream_limit_req_module);

    if (sctx == NULL || sctx->status == 0) {
        v->not_found = 1;
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = ngx_stream_limit_req_status[sctx->status - 1].len;
    v->data = ngx_stream_limit_req_status[sctx->status - 1].data;

    return NGX_OK;
}


static void
ngx_stream_limit_req_unlock(ngx_stream_limit_req_limit_t *limits, ngx_uint_t n)
{
    ngx_stream_limit_req_ctx_t  *ctx;

    while (n--) {
        ctx = limits[n].shm_zone->data;

        if (ctx->node == NULL) {
            continue;
        }

        ngx_shmtx_lock(&ctx->shpool->mutex);

        ctx->node->count--;

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        ctx->node = NULL;
    }
}


static void
ngx_stream_limit_req_cleanup(void *data)
{
    ngx_stream_limit_req_session_ctx_t  *sctx = data;

    /*
     * The timer sits in the event tree and its memory belongs to the
     * connection pool: a connection going away during the delay would
     * leave the tree pointing into freed memory.
     */
    if (sctx->event.timer_set) {
        ngx_del_timer(&sctx->event);
    }
}


static void
ngx_stream_limit_req_delay(ngx_event_t *ev)
{
    ngx_connection_t                    *c = ev->data;
    ngx_stream_session_t                *s = c->data;
    ngx_stream_limit_req_session_ctx_t  *sctx;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream limit_req delay is over");

    sctx = ngx_stream_get_module_ctx(s, ngx_stream_limit_req_module);
    if (sctx == NULL) {
        ngx_stream_finalize_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
        return;
    }

    /*
     * The phase engine does not advance past a handler that returned
     * NGX_AGAIN, so the handler runs again on the next walk.  These flags
     * say the accounting has been done and the wait is over.
     */
    sctx->delayed = 0;
    sctx->handled = 1;

    ngx_stream_core_run_phases(s);
}


static ngx_int_t
ngx_stream_limit_req_handler(ngx_stream_session_t *s)
{
    uint32_t                             hash;
    ngx_str_t                            key;
    ngx_int_t                            rc;
    ngx_uint_t                           n, excess;
    ngx_msec_t                           delay;
    ngx_pool_cleanup_t                  *cln;
    ngx_connection_t                    *c;
    ngx_stream_limit_req_ctx_t          *ctx;
    ngx_stream_limit_req_limit_t        *limit, *limits;
    ngx_stream_limit_req_srv_conf_t     *lrcf;
    ngx_stream_limit_req_session_ctx_t  *sctx;

    c = s->connection;

    lrcf = ngx_stream_get_module_srv_conf(s, ngx_stream_limit_req_module);

    if (lrcf->limits.nelts == 0) {
        return NGX_DECLINED;
    }

    sctx = ngx_stream_get_module_ctx(s, ngx_stream_limit_req_module);

    if (sctx != NULL) {

        if (sctx->handled) {
            return NGX_DECLINED;
        }

        /*
         * The connection is parked on the delay timer.  Anything that walks
         * the phases again before the timer fires, payload arriving on an
         * edge or a session resumed from elsewhere, must not count a second
         * time: the rate would then be charged once per event instead of
         * once per connection.
         */
        if (sctx->delayed) {
            return NGX_AGAIN;
        }
    }

    if (sctx == NULL) {
        sctx = ngx_pcalloc(c->pool,
                           sizeof(ngx_stream_limit_req_session_ctx_t));
        if (sctx == NULL) {
            return NGX_ERROR;
        }

        cln = ngx_pool_cleanup_add(c->pool, 0);
        if (cln == NULL) {
            return NGX_ERROR;
        }

        cln->handler = ngx_stream_limit_req_cleanup;
        cln->data = sctx;

        ngx_stream_set_ctx(s, sctx, ngx_stream_limit_req_module);
    }

    limits = lrcf->limits.elts;

    excess = 0;
    rc = NGX_DECLINED;
    limit = NULL;
    n = 0;

    for (n = 0; n < lrcf->limits.nelts; n++) {

        limit = &limits[n];

        ctx = limit->shm_zone->data;

        if (ngx_stream_complex_value(s, &ctx->key, &key) != NGX_OK) {
            ngx_stream_limit_req_unlock(limits, n);
            return NGX_ERROR;
        }

        if (key.len == 0) {
            continue;
        }

        if (key.len > 65535) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "the value of the \"%V\" key "
                          "is more than 65535 bytes: \"%V\"",
                          &ctx->key.value, &key);
            continue;
        }

        hash = ngx_crc32_short(key.data, key.len);

        ngx_shmtx_lock(&ctx->shpool->mutex);

        rc = ngx_stream_limit_req_lookup(limit, hash, &key, &excess,
                                         (n == lrcf->limits.nelts - 1));

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        ngx_log_debug4(NGX_LOG_DEBUG_STREAM, c->log, 0,
                       "stream limit_req[%ui]: %i %ui.%03ui",
                       n, rc, excess / 1000, excess % 1000);

        if (rc != NGX_AGAIN) {
            break;
        }
    }

    if (rc == NGX_DECLINED) {
        return NGX_DECLINED;
    }

    if (rc == NGX_BUSY || rc == NGX_ERROR) {

        if (rc == NGX_BUSY) {
            ngx_log_error(lrcf->limit_log_level, c->log, 0,
                          "%slimiting connections by zone \"%V\", "
                          "excess: %ui.%03ui",
                          lrcf->dry_run ? "dry run " : "",
                          &limit->shm_zone->shm.name,
                          excess / 1000, excess % 1000);
        }

        /*
         * Whatever was counted on the way here has to be given back, or a
         * refused connection raises the excess of the zones in front of
         * the one that refused it.
         */
        ngx_stream_limit_req_unlock(limits, n);

        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }

        if (lrcf->dry_run) {
            sctx->status = NGX_STREAM_LIMIT_REQ_REJECTED_DRY_RUN;
            sctx->handled = 1;
            return NGX_DECLINED;
        }

        sctx->status = NGX_STREAM_LIMIT_REQ_REJECTED;

        return NGX_STREAM_SERVICE_UNAVAILABLE;
    }

    /* rc == NGX_OK || rc == NGX_AGAIN */

    if (rc == NGX_AGAIN) {
        excess = 0;
    }

    delay = ngx_stream_limit_req_account(limits, n, &excess, &limit);

    if (!delay) {
        sctx->status = NGX_STREAM_LIMIT_REQ_PASSED;
        sctx->handled = 1;
        return NGX_DECLINED;
    }

    ngx_log_error(lrcf->delay_log_level, c->log, 0,
                  "%sdelaying connection, excess: %ui.%03ui, by zone \"%V\"",
                  lrcf->dry_run ? "dry run " : "",
                  excess / 1000, excess % 1000, &limit->shm_zone->shm.name);

    if (lrcf->dry_run) {
        sctx->status = NGX_STREAM_LIMIT_REQ_DELAYED_DRY_RUN;
        sctx->handled = 1;
        return NGX_DECLINED;
    }

    sctx->status = NGX_STREAM_LIMIT_REQ_DELAYED;
    sctx->delayed = 1;

    sctx->event.handler = ngx_stream_limit_req_delay;
    sctx->event.log = c->log;

    /*
     * The connection, not the session: ngx_add_timer() and ngx_del_timer()
     * read ev->data as an ngx_connection_t to name the descriptor in a debug
     * log, so anything else there is a wrong number at best.
     */
    sctx->event.data = c;

    ngx_add_timer(&sctx->event, delay);

    return NGX_AGAIN;
}


static void
ngx_stream_limit_req_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t            **p;
    ngx_stream_limit_req_node_t   *lrn, *lrnt;

    for ( ;; ) {

        if (node->key < temp->key) {
            p = &temp->left;

        } else if (node->key > temp->key) {
            p = &temp->right;

        } else { /* node->key == temp->key */

            lrn = (ngx_stream_limit_req_node_t *) &node->color;
            lrnt = (ngx_stream_limit_req_node_t *) &temp->color;

            p = (ngx_memn2cmp(lrn->data, lrnt->data, lrn->len, lrnt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_int_t
ngx_stream_limit_req_lookup(ngx_stream_limit_req_limit_t *limit,
    ngx_uint_t hash, ngx_str_t *key, ngx_uint_t *ep, ngx_uint_t account)
{
    size_t                        size;
    ngx_int_t                     rc, excess;
    ngx_msec_t                    now;
    ngx_msec_int_t                ms;
    ngx_rbtree_node_t            *node, *sentinel;
    ngx_stream_limit_req_ctx_t   *ctx;
    ngx_stream_limit_req_node_t  *lr;

    now = ngx_current_msec;

    ctx = limit->shm_zone->data;

    node = ctx->sh->rbtree.root;
    sentinel = ctx->sh->rbtree.sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        lr = (ngx_stream_limit_req_node_t *) &node->color;

        rc = ngx_memn2cmp(key->data, lr->data, key->len, (size_t) lr->len);

        if (rc == 0) {
            ngx_queue_remove(&lr->queue);
            ngx_queue_insert_head(&ctx->sh->queue, &lr->queue);

            ms = (ngx_msec_int_t) (now - lr->last);

            if (ms < -60000) {
                /*
                 * The clock went backwards by more than a minute: trust
                 * the stored stamp rather than a negative age, which would
                 * let a burst through.
                 */
                ms = 1;

            } else if (ms < 0) {
                ms = 0;
            }

            excess = lr->excess - ctx->rate * ms / 1000 + 1000;

            if (excess < 0) {
                excess = 0;
            }

            *ep = excess;

            if ((ngx_uint_t) excess > limit->burst) {
                return NGX_BUSY;
            }

            if (account) {
                lr->excess = excess;

                if (ms) {
                    lr->last = now;
                }

                return NGX_OK;
            }

            lr->count++;

            ctx->node = lr;

            return NGX_AGAIN;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    *ep = 0;

    size = offsetof(ngx_rbtree_node_t, color)
           + offsetof(ngx_stream_limit_req_node_t, data)
           + key->len;

    ngx_stream_limit_req_expire(ctx, 1);

    node = ngx_slab_alloc_locked(ctx->shpool, size);

    if (node == NULL) {
        ngx_stream_limit_req_expire(ctx, 0);

        node = ngx_slab_alloc_locked(ctx->shpool, size);
        if (node == NULL) {
            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                          "could not allocate node%s", ctx->shpool->log_ctx);
            return NGX_ERROR;
        }
    }

    node->key = hash;

    lr = (ngx_stream_limit_req_node_t *) &node->color;

    lr->len = (u_short) key->len;
    lr->excess = 0;

    ngx_memcpy(lr->data, key->data, key->len);

    ngx_rbtree_insert(&ctx->sh->rbtree, node);

    ngx_queue_insert_head(&ctx->sh->queue, &lr->queue);

    if (account) {
        lr->last = now;
        lr->count = 0;
        return NGX_OK;
    }

    lr->last = 0;
    lr->count = 1;

    ctx->node = lr;

    return NGX_AGAIN;
}


static ngx_msec_t
ngx_stream_limit_req_account(ngx_stream_limit_req_limit_t *limits,
    ngx_uint_t n, ngx_uint_t *ep, ngx_stream_limit_req_limit_t **limit)
{
    ngx_int_t                     excess;
    ngx_msec_t                    now, delay, max_delay;
    ngx_msec_int_t                ms;
    ngx_stream_limit_req_ctx_t   *ctx;
    ngx_stream_limit_req_node_t  *lr;

    excess = *ep;

    if ((ngx_uint_t) excess <= (*limit)->delay) {
        max_delay = 0;

    } else {
        ctx = (*limit)->shm_zone->data;
        max_delay = (excess - (*limit)->delay) * 1000 / ctx->rate;
    }

    while (n--) {
        ctx = limits[n].shm_zone->data;
        lr = ctx->node;

        if (lr == NULL) {
            continue;
        }

        ngx_shmtx_lock(&ctx->shpool->mutex);

        now = ngx_current_msec;
        ms = (ngx_msec_int_t) (now - lr->last);

        if (ms < -60000) {
            ms = 1;

        } else if (ms < 0) {
            ms = 0;
        }

        excess = lr->excess - ctx->rate * ms / 1000 + 1000;

        if (excess < 0) {
            excess = 0;
        }

        if (ms) {
            lr->last = now;
        }

        lr->excess = excess;
        lr->count--;

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        ctx->node = NULL;

        if ((ngx_uint_t) excess <= limits[n].delay) {
            continue;
        }

        delay = (excess - limits[n].delay) * 1000 / ctx->rate;

        if (delay > max_delay) {
            max_delay = delay;
            *ep = excess;
            *limit = &limits[n];
        }
    }

    return max_delay;
}


static void
ngx_stream_limit_req_expire(ngx_stream_limit_req_ctx_t *ctx, ngx_uint_t n)
{
    ngx_int_t                     excess;
    ngx_msec_t                    now;
    ngx_queue_t                  *q;
    ngx_msec_int_t                ms;
    ngx_rbtree_node_t            *node;
    ngx_stream_limit_req_node_t  *lr;

    now = ngx_current_msec;

    /*
     * n == 1 deletes one or two zero rate entries, n == 0 deletes oldest
     * entry by force and one or two zero rate entries -- the same rule the
     * http module follows, so a zone behaves the same in either block.
     */

    while (n < 3) {

        if (ngx_queue_empty(&ctx->sh->queue)) {
            return;
        }

        q = ngx_queue_last(&ctx->sh->queue);

        lr = ngx_queue_data(q, ngx_stream_limit_req_node_t, queue);

        if (lr->count) {
            /*
             * There is not much sense to look further, because we bump
             * nodes on the lookup stage.
             */
            return;
        }

        if (n++ != 0) {

            ms = (ngx_msec_int_t) (now - lr->last);
            ms = ngx_abs(ms);

            if (ms < 60000) {
                return;
            }

            excess = lr->excess - ctx->rate * ms / 1000;

            if (excess > 0) {
                return;
            }
        }

        ngx_queue_remove(q);

        node = (ngx_rbtree_node_t *)
                   ((u_char *) lr - offsetof(ngx_rbtree_node_t, color));

        ngx_rbtree_delete(&ctx->sh->rbtree, node);

        ngx_slab_free_locked(ctx->shpool, node);
    }
}


static ngx_int_t
ngx_stream_limit_req_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_stream_limit_req_ctx_t  *octx = data;

    size_t                       len;
    ngx_stream_limit_req_ctx_t  *ctx;

    ctx = shm_zone->data;

    if (octx) {
        if (ctx->key.value.len != octx->key.value.len
            || ngx_strncmp(ctx->key.value.data, octx->key.value.data,
                           ctx->key.value.len)
               != 0)
        {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "limit_req \"%V\" uses the \"%V\" key "
                          "while previously it used the \"%V\" key",
                          &shm_zone->shm.name, &ctx->key.value,
                          &octx->key.value);
            return NGX_ERROR;
        }

        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;
        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool,
                             sizeof(ngx_stream_limit_req_shctx_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_stream_limit_req_rbtree_insert_value);

    ngx_queue_init(&ctx->sh->queue);

    len = sizeof(" in limit_req zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in limit_req zone \"%V\"%Z",
                &shm_zone->shm.name);

    ctx->shpool->log_nomem = 0;

    return NGX_OK;
}


static void *
ngx_stream_limit_req_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_limit_req_srv_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_limit_req_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->limits.elts = NULL;
     */

    conf->limit_log_level = NGX_CONF_UNSET_UINT;
    conf->dry_run = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_stream_limit_req_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_stream_limit_req_srv_conf_t *prev = parent;
    ngx_stream_limit_req_srv_conf_t *conf = child;

    if (conf->limits.elts == NULL) {
        conf->limits = prev->limits;
    }

    ngx_conf_merge_uint_value(conf->limit_log_level, prev->limit_log_level,
                              NGX_LOG_ERR);

    conf->delay_log_level = (conf->limit_log_level == NGX_LOG_INFO) ?
                                NGX_LOG_INFO : conf->limit_log_level + 1;

    ngx_conf_merge_value(conf->dry_run, prev->dry_run, 0);

    return NGX_CONF_OK;
}


static char *
ngx_stream_limit_req_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char                             *p;
    size_t                              len;
    ssize_t                             size;
    ngx_str_t                          *value, name, s;
    ngx_int_t                           rate, scale;
    ngx_uint_t                          i;
    ngx_shm_zone_t                     *shm_zone;
    ngx_stream_limit_req_ctx_t         *ctx;
    ngx_stream_compile_complex_value_t  ccv;

    value = cf->args->elts;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_stream_limit_req_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_stream_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &ctx->key;

    if (ngx_stream_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    size = 0;
    rate = 1;
    scale = 1;
    name.len = 0;
    name.data = NULL;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            name.data = value[i].data + 5;

            p = (u_char *) ngx_strchr(name.data, ':');

            if (p == NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            name.len = p - name.data;

            s.data = p + 1;
            s.len = value[i].data + value[i].len - s.data;

            size = ngx_parse_size(&s);

            if (size == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            if (size < (ssize_t) (8 * ngx_pagesize)) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "zone \"%V\" is too small", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "rate=", 5) == 0) {

            len = value[i].len;
            p = value[i].data + len - 3;

            if (ngx_strncmp(p, "r/s", 3) == 0) {
                scale = 1;
                len -= 3;

            } else if (ngx_strncmp(p, "r/m", 3) == 0) {
                scale = 60;
                len -= 3;
            }

            rate = ngx_atoi(value[i].data + 5, len - 5);
            if (rate <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid rate \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    ctx->rate = rate * 1000 / scale;

    shm_zone = ngx_shared_memory_add(cf, &name, size,
                                     &ngx_stream_limit_req_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {
        ctx = shm_zone->data;

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "%V \"%V\" is already bound to key \"%V\"",
                           &cmd->name, &name, &ctx->key.value);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_stream_limit_req_init_zone;
    shm_zone->data = ctx;

    return NGX_CONF_OK;
}


static char *
ngx_stream_limit_req(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_limit_req_srv_conf_t  *lrcf = conf;

    ngx_int_t                      burst, delay;
    ngx_str_t                     *value, s;
    ngx_uint_t                     i;
    ngx_shm_zone_t                *shm_zone;
    ngx_stream_limit_req_limit_t  *limit, *limits;

    value = cf->args->elts;

    shm_zone = NULL;
    burst = 0;
    delay = 0;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            shm_zone = ngx_shared_memory_add(cf, &s, 0,
                                             &ngx_stream_limit_req_module);
            if (shm_zone == NULL) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "burst=", 6) == 0) {

            burst = ngx_atoi(value[i].data + 6, value[i].len - 6);
            if (burst <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid burst value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "delay=", 6) == 0) {

            delay = ngx_atoi(value[i].data + 6, value[i].len - 6);
            if (delay <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid delay value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strcmp(value[i].data, "nodelay") == 0) {
            delay = NGX_MAX_INT_T_VALUE / 1000;
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (shm_zone == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter", &cmd->name);
        return NGX_CONF_ERROR;
    }

    limits = lrcf->limits.elts;

    if (limits == NULL) {
        if (ngx_array_init(&lrcf->limits, cf->pool, 1,
                           sizeof(ngx_stream_limit_req_limit_t))
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
    }

    for (i = 0; i < lrcf->limits.nelts; i++) {
        if (shm_zone == limits[i].shm_zone) {
            return "is duplicate";
        }
    }

    limit = ngx_array_push(&lrcf->limits);
    if (limit == NULL) {
        return NGX_CONF_ERROR;
    }

    limit->shm_zone = shm_zone;
    limit->burst = burst * 1000;
    limit->delay = delay * 1000;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_stream_limit_req_init(ngx_conf_t *cf)
{
    ngx_stream_handler_pt        *h;
    ngx_stream_core_main_conf_t  *cmcf;

    cmcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_STREAM_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_stream_limit_req_handler;

    return NGX_OK;
}
