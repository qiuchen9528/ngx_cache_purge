#ifndef NGX_CACHE_PURGE_JOB_H
#define NGX_CACHE_PURGE_JOB_H

#include <ngx_config.h>
#include <nginx.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define NGX_CACHE_PURGE_JOB_ID_LEN 26
#define NGX_CACHE_PURGE_JOB_DEFAULT_MAX 10000
#define NGX_CACHE_PURGE_JOB_DEFAULT_TTL 86400  /* 24h in seconds */

typedef enum {
    NGX_CACHE_PURGE_JOB_QUEUED = 0,
    NGX_CACHE_PURGE_JOB_PROCESSING,
    NGX_CACHE_PURGE_JOB_COMPLETED,
    NGX_CACHE_PURGE_JOB_FAILED
} ngx_cache_purge_job_status_e;

typedef enum {
    NGX_CACHE_PURGE_TYPE_EXACT = 0,
    NGX_CACHE_PURGE_TYPE_WILDCARD,
    NGX_CACHE_PURGE_TYPE_ALL
} ngx_cache_purge_type_e;

/* shm job: rbtree node MUST be first field so (job*) == (node*) cast is safe */
typedef struct {
    ngx_rbtree_node_t  node;   /* key = uint64 job id numeric */
    u_char             job_id[NGX_CACHE_PURGE_JOB_ID_LEN + 1];

    ngx_uint_t         status;
    ngx_uint_t         purge_type;

    time_t             created_at;
    time_t             started_at;
    time_t             completed_at;

    /* millisecond timestamps for duration (ngx_current_msec, monotonic
     * elapsed).  time_t fields above stay for API compat/display.
     * Appended at struct end so reload-reused shm with the same binary
     * keeps layout; a binary upgrade must use restart/hot-upgrade
     * (fresh shm), not same-shm reuse across different layouts. */
    ngx_msec_t         created_msec;
    ngx_msec_t         started_msec;
    ngx_msec_t         completed_msec;
    ngx_msec_t         duration_ms;

    uint64_t           files_deleted;

    ngx_int_t          error_code;

    /* key/zone blob allocated separately in same shm slab; pointer valid
     * across workers (shm mapped same addr after fork) */
    u_char            *key_zone;
    uint32_t           key_len;
    uint32_t           zone_len;
    uint32_t           key_offset;   /* offset of key inside key_zone */
} ngx_cache_purge_job_shm_t;

typedef struct {
    ngx_slab_pool_t  *shpool;
    ngx_shmtx_sh_t    sh;
    ngx_shmtx_t       mutex;
    ngx_rbtree_t      rbtree;
    ngx_rbtree_node_t sentinel;
    ngx_uint_t        nelts;
    ngx_uint_t        max_jobs;
    time_t            ttl;          /* seconds */
} ngx_cache_purge_job_store_t;

typedef struct {
    ngx_str_t         zone_name;
    size_t            zone_size;
    ngx_uint_t        max_jobs;
    ngx_msec_t        ttl;          /* msec as parsed */
    ngx_flag_t        enable;
    ngx_cache_purge_job_store_t *store;
} ngx_cache_purge_job_main_conf_t;

/* global store set by shm init; visible to module.c */
extern ngx_cache_purge_job_store_t *ngx_cache_purge_job_store;

ngx_int_t ngx_cache_purge_job_shm_init(ngx_shm_zone_t *shm_zone, void *data);

uint64_t ngx_cache_purge_job_generate_id(ngx_cycle_t *cycle);
u_char *ngx_cache_purge_job_ulid_encode(uint64_t id, u_char *buf);
ngx_int_t ngx_cache_purge_job_ulid_decode(u_char *s, uint64_t *out);

ngx_cache_purge_job_shm_t *ngx_cache_purge_job_create(ngx_cycle_t *cycle,
    ngx_cache_purge_job_store_t *store, ngx_str_t *key, ngx_str_t *zone,
    ngx_uint_t purge_type);
ngx_cache_purge_job_shm_t *ngx_cache_purge_job_lookup(ngx_cache_purge_job_store_t *store,
    uint64_t job_id);
ngx_int_t ngx_cache_purge_job_set_processing(ngx_cache_purge_job_store_t *store,
    ngx_cache_purge_job_shm_t *job);
ngx_int_t ngx_cache_purge_job_set_completed(ngx_cache_purge_job_store_t *store,
    ngx_cache_purge_job_shm_t *job, uint64_t files_deleted);
ngx_int_t ngx_cache_purge_job_set_failed(ngx_cache_purge_job_store_t *store,
    ngx_cache_purge_job_shm_t *job, ngx_int_t error_code);
ngx_int_t ngx_cache_purge_job_cleanup(ngx_cycle_t *cycle);

/* lock-protected snapshot for response rendering; avoids holding the
 * shm lock across pool allocs/output.  Returns NGX_OK or NGX_ERROR
 * (store NULL / job not found). */
typedef struct {
    ngx_uint_t         status;
    ngx_uint_t         purge_type;
    uint64_t           files_deleted;
    ngx_msec_t         duration_ms;
    ngx_msec_t         started_msec;
    ngx_int_t          error_code;
    time_t             created_at;
    time_t             started_at;
    time_t             completed_at;
} ngx_cache_purge_job_snapshot_t;

ngx_int_t ngx_cache_purge_job_snapshot(ngx_cache_purge_job_store_t *store,
    uint64_t job_id, ngx_cache_purge_job_snapshot_t *snap);

#endif
