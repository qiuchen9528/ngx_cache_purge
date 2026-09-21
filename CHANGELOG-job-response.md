# Job Response Fix

## Fix

When `cache_purge_job_store` is enabled, PURGE requests that create a Job now
return the Job API JSON response automatically, even when
`cache_purge_response_type` is left at its legacy default (`html`).

Before:

```text
Content-Type: text/html

<html>...Status: purged...</html>
```

After:

```json
{"job_id":"...","status":"completed","type":"url","key":"...","files_deleted":1}
```

For background jobs the response remains `202 Accepted` and includes the
`Location: /purge/status/<job_id>` header.

Requests that do not carry a Job ID retain the configured legacy response
format.

## Configuration

No configuration change is required for Job JSON responses. The following is
still recommended explicitly for a machine-facing PURGE API:

```nginx
cache_purge_response_type json;
cache_purge_job_store zone=cache_purge_jobs:20m max_jobs=10000 ttl=24h;
```
