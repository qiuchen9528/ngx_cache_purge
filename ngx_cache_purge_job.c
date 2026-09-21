#include "ngx_cache_purge_job.h"

ngx_cache_purge_job_store_t *ngx_cache_purge_job_store = NULL;

static void ngx_cache_purge_job_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static void ngx_cache_purge_job_cleanup_expired(
    ngx_cache_purge_job_store_t *store, time_t now);


ngx_int_t
ngx_cache_purge_job_shm_init(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_cache_purge_job_main_conf_t *jmcf = shm_zone->data;
    ngx_cache_purge_job_main_conf_t *old = data;
    ngx_cache_purge_job_store_t *store;
    ngx_slab_pool_t *shpool;

    if (old != NULL && old->store != NULL) {
        jmcf->store = old->store;
        ngx_cache_purge_job_store = old->store;
        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    store = ngx_slab_calloc(shpool, sizeof(ngx_cache_purge_job_store_t));
    if (store == NULL) {
        ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                      "ngx_cache_purge_job: could not allocate job store");
        return NGX_ERROR;
    }

    store->shpool = shpool;
    store->max_jobs = jmcf->max_jobs ? jmcf->max_jobs
                                     : NGX_CACHE_PURGE_JOB_DEFAULT_MAX;
    store->ttl = (time_t) (jmcf->ttl / 1000);
    if (store->ttl == 0) {
        store->ttl = NGX_CACHE_PURGE_JOB_DEFAULT_TTL;
    }

    if (ngx_shmtx_create(&store->mutex, &store->sh, NULL) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_rbtree_init(&store->rbtree, &store->sentinel,
                    ngx_cache_purge_job_rbtree_insert_value);

    jmcf->store = store;
    ngx_cache_purge_job_store = store;

    return NGX_OK;
}


static void
ngx_cache_purge_job_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t **p;

    for (;;) {
        if (node->key < temp->key) {
            p = &temp->left;

        } else if (node->key > temp->key) {
            p = &temp->right;

        } else { /* same numeric id: tie-break on ULID string */
            ngx_cache_purge_job_shm_t *job =
                (ngx_cache_purge_job_shm_t *) node;
            ngx_cache_purge_job_shm_t *jt =
                (ngx_cache_purge_job_shm_t *) temp;

            if (ngx_memcmp(job->job_id, jt->job_id,
                           NGX_CACHE_PURGE_JOB_ID_LEN) < 0)
            {
                p = &temp->left;
            } else {
                p = &temp->right;
            }
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


uint64_t
ngx_cache_purge_job_generate_id(ngx_cycle_t *cycle)
{
    static ngx_atomic_t counter = 0;
    uint64_t id;
    time_t sec;
    ngx_msec_t msec;
    ngx_uint_t n;

    ngx_time_update();
    sec = ngx_time();
    msec = ngx_current_msec % 1000;
    n = (ngx_uint_t) ngx_atomic_fetch_add(&counter, 1);

    /* 32b sec | 10b msec | 10b pid | 12b counter : multi-worker unique */
    id = ((uint64_t) (uint32_t) sec << 32)
         | ((uint64_t) (msec & 0x3FF) << 22)
         | ((uint64_t) ((ngx_pid & 0x3FF)) << 12)
         | ((uint64_t) (n & 0xFFF));

    if (id == 0) {
        id = 1;
    }

    return id;
}


u_char *
ngx_cache_purge_job_ulid_encode(uint64_t id, u_char *buf)
{
    static const u_char encoding[] =
        "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    int i;

    for (i = NGX_CACHE_PURGE_JOB_ID_LEN - 1; i >= 0; i--) {
        buf[i] = encoding[id & 0x1F];
        id >>= 5;
    }
    buf[NGX_CACHE_PURGE_JOB_ID_LEN] = '\0';

    return buf;
}


ngx_int_t
ngx_cache_purge_job_ulid_decode(u_char *s, uint64_t *out)
{
    static const char *alpha = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    uint64_t id = 0;
    ngx_int_t i, k;

    for (i = 0; i < NGX_CACHE_PURGE_JOB_ID_LEN; i++) {
        u_char c = s[i];
        if (c >= 'a' && c <= 'z') {
            c -= ('a' - 'A');
        }

        id <<= 5;

        for (k = 0; k < 32; k++) {
            if (alpha[k] == (char) c) {
                id |= (uint64_t) k;
                break;
            }
        }

        if (k == 32) {
            return NGX_ERROR;
        }
    }

    *out = id;
    return NGX_OK;
}


ngx_cache_purge_job_shm_t *
ngx_cache_purge_job_create(ngx_cycle_t *cycle, ngx_cache_purge_job_store_t *store,
    ngx_str_t *key, ngx_str_t *zone, ngx_uint_t purge_type)
{
    ngx_cache_purge_job_shm_t *job;
    uint64_t job_id;
    u_char *p;
    size_t zone_len, key_len;

    if (store == NULL) {
        return NULL;
    }

    zone_len = (zone != NULL) ? zone->len : 0;
    key_len = (key != NULL) ? key->len : 0;

    ngx_shmtx_lock(&store->mutex);

    if (store->nelts >= store->max_jobs) {
        ngx_cache_purge_job_cleanup_expired(store, ngx_time());
        if (store->nelts >= store->max_jobs) {
            ngx_shmtx_unlock(&store->mutex);
            return NULL;
        }
    }

    job = ngx_slab_calloc(store->shpool, sizeof(ngx_cache_purge_job_shm_t));
    if (job == NULL) {
        ngx_shmtx_unlock(&store->mutex);
        return NULL;
    }

    job_id = ngx_cache_purge_job_generate_id(cycle);
    ngx_cache_purge_job_ulid_encode(job_id, job->job_id);

    job->status = NGX_CACHE_PURGE_JOB_QUEUED;
    job->purge_type = purge_type;
    job->created_at = ngx_time();
    job->created_msec = ngx_current_msec;
    job->started_at = 0;
    job->started_msec = 0;
    job->completed_at = 0;
    job->completed_msec = 0;
    job->duration_ms = 0;
    job->files_deleted = 0;
    job->error_code = 0;
    job->key_len = (uint32_t) key_len;
    job->zone_len = (uint32_t) zone_len;
    job->key_offset = (uint32_t) zone_len + 1;
    job->key_zone = NULL;

    if (zone_len + key_len + 2 > 0) {
        p = ngx_slab_alloc(store->shpool, zone_len + key_len + 2);
        if (p == NULL) {
            ngx_slab_free(store->shpool, job);
            ngx_shmtx_unlock(&store->mutex);
            return NULL;
        }

        if (zone_len > 0) {
            ngx_memcpy(p, zone->data, zone_len);
        }
        p[zone_len] = '|';
        if (key_len > 0) {
            ngx_memcpy(p + zone_len + 1, key->data, key_len);
        }
        p[zone_len + 1 + key_len] = '\0';
        job->key_zone = p;
    }

    job->node.key = job_id;
    ngx_rbtree_insert(&store->rbtree, &job->node);
    store->nelts++;

    ngx_shmtx_unlock(&store->mutex);

    return job;
}


/* caller must hold store->mutex */
static ngx_cache_purge_job_shm_t *
ngx_cache_purge_job_find_locked(ngx_cache_purge_job_store_t *store,
    uint64_t job_id)
{
    ngx_rbtree_node_t *node, *sentinel;

    node = store->rbtree.root;
    sentinel = store->rbtree.sentinel;

    while (node != sentinel) {
        if (job_id < node->key) {
            node = node->left;
            continue;
        }

        if (job_id > node->key) {
            node = node->right;
            continue;
        }

        return (ngx_cache_purge_job_shm_t *) node;
    }

    return NULL;
}


ngx_cache_purge_job_shm_t *
ngx_cache_purge_job_lookup(ngx_cache_purge_job_store_t *store, uint64_t job_id)
{
    ngx_cache_purge_job_shm_t *job;

    if (store == NULL || job_id == 0) {
        return NULL;
    }

    ngx_shmtx_lock(&store->mutex);
    job = ngx_cache_purge_job_find_locked(store, job_id);
    ngx_shmtx_unlock(&store->mutex);

    return job;
}


ngx_int_t
ngx_cache_purge_job_snapshot(ngx_cache_purge_job_store_t *store,
    uint64_t job_id, ngx_cache_purge_job_snapshot_t *snap)
{
    ngx_cache_purge_job_shm_t *job;

    if (store == NULL || job_id == 0 || snap == NULL) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&store->mutex);

    job = ngx_cache_purge_job_find_locked(store, job_id);
    if (job == NULL) {
        ngx_shmtx_unlock(&store->mutex);
        return NGX_ERROR;
    }

    snap->status = job->status;
    snap->purge_type = job->purge_type;
    snap->files_deleted = job->files_deleted;
    snap->error_code = job->error_code;
    snap->created_at = job->created_at;
    snap->started_at = job->started_at;
    snap->completed_at = job->completed_at;
    snap->started_msec = job->started_msec;

    if (job->status == NGX_CACHE_PURGE_JOB_COMPLETED
        || job->status == NGX_CACHE_PURGE_JOB_FAILED)
    {
        snap->duration_ms = job->duration_ms;
    } else if (job->started_msec != 0
               && ngx_current_msec >= job->started_msec)
    {
        /* in-flight: live elapsed */
        snap->duration_ms = ngx_current_msec - job->started_msec;
    } else {
        snap->duration_ms = 0;
    }

    ngx_shmtx_unlock(&store->mutex);

    return NGX_OK;
}


ngx_int_t
ngx_cache_purge_job_set_processing(ngx_cache_purge_job_store_t *store,
    ngx_cache_purge_job_shm_t *job)
{
    if (store == NULL || job == NULL) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&store->mutex);

    if (job->status == NGX_CACHE_PURGE_JOB_QUEUED) {
        job->status = NGX_CACHE_PURGE_JOB_PROCESSING;
        job->started_at = ngx_time();
        job->started_msec = ngx_current_msec;
    }

    ngx_shmtx_unlock(&store->mutex);
    return NGX_OK;
}


ngx_int_t
ngx_cache_purge_job_set_completed(ngx_cache_purge_job_store_t *store,
    ngx_cache_purge_job_shm_t *job, uint64_t files_deleted)
{
    if (store == NULL || job == NULL) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&store->mutex);

    /* allow queued->completed as well (sync exact purge path) */
    if (job->status == NGX_CACHE_PURGE_JOB_PROCESSING
        || job->status == NGX_CACHE_PURGE_JOB_QUEUED)
    {
        if (job->started_at == 0) {
            job->started_at = ngx_time();
            job->started_msec = ngx_current_msec;
        }
        /* refresh cached time: ngx_current_msec is per-event-loop-iteration,
         * and a whole background walk may finish inside one iteration */
        ngx_time_update();
        job->completed_at = ngx_time();
        job->completed_msec = ngx_current_msec;
        if (job->completed_msec >= job->started_msec) {
            job->duration_ms = job->completed_msec - job->started_msec;
        } else {
            job->duration_ms = 0;
        }
        job->status = NGX_CACHE_PURGE_JOB_COMPLETED;
        job->files_deleted = files_deleted;
    }

    ngx_shmtx_unlock(&store->mutex);
    return NGX_OK;
}


ngx_int_t
ngx_cache_purge_job_set_failed(ngx_cache_purge_job_store_t *store,
    ngx_cache_purge_job_shm_t *job, ngx_int_t error_code)
{
    if (store == NULL || job == NULL) {
        return NGX_ERROR;
    }

    ngx_shmtx_lock(&store->mutex);

    if (job->status == NGX_CACHE_PURGE_JOB_QUEUED
        || job->status == NGX_CACHE_PURGE_JOB_PROCESSING)
    {
        if (job->started_msec == 0) {
            job->started_at = ngx_time();
            job->started_msec = ngx_current_msec;
        }
        /* refresh cached time: see set_completed */
        ngx_time_update();
        job->completed_at = ngx_time();
        job->completed_msec = ngx_current_msec;
        if (job->completed_msec >= job->started_msec) {
            job->duration_ms = job->completed_msec - job->started_msec;
        } else {
            job->duration_ms = 0;
        }
        job->status = NGX_CACHE_PURGE_JOB_FAILED;
        job->error_code = error_code;
    }

    ngx_shmtx_unlock(&store->mutex);
    return NGX_OK;
}


static void
ngx_cache_purge_job_cleanup_expired(ngx_cache_purge_job_store_t *store, time_t now)
{
    ngx_rbtree_node_t *node, *sentinel, *next;
    ngx_cache_purge_job_shm_t *job;

    /* caller holds store->mutex */
    node = ngx_rbtree_min(store->rbtree.root, &store->sentinel);
    sentinel = &store->sentinel;

    while (node != sentinel) {
        next = ngx_rbtree_next(&store->rbtree, node);
        job = (ngx_cache_purge_job_shm_t *) node;

        if ((job->status == NGX_CACHE_PURGE_JOB_COMPLETED
             || job->status == NGX_CACHE_PURGE_JOB_FAILED)
            && (now - job->completed_at > store->ttl))
        {
            ngx_rbtree_delete(&store->rbtree, node);
            if (store->nelts > 0) {
                store->nelts--;
            }
            if (job->key_zone != NULL) {
                ngx_slab_free(store->shpool, job->key_zone);
            }
            ngx_slab_free(store->shpool, job);
        }

        node = next;
        if (node == NULL) {
            break;
        }
    }
}


ngx_int_t
ngx_cache_purge_job_cleanup(ngx_cycle_t *cycle)
{
    if (ngx_cache_purge_job_store != NULL) {
        ngx_shmtx_lock(&ngx_cache_purge_job_store->mutex);
        ngx_cache_purge_job_cleanup_expired(ngx_cache_purge_job_store,
                                            ngx_time());
        ngx_shmtx_unlock(&ngx_cache_purge_job_store->mutex);
    }
    return NGX_OK;
}
