# Changelog

## v0.1.0 (2026-10-20)

- First release: `limit_req_zone` and `limit_req` for the stream subsystem,
  with `burst`, `nodelay`, `delay=`, `limit_req_log_level` and
  `limit_req_dry_run`, spelled and counted as in the http module.
- The handler runs in the preaccess phase, so a refused connection is
  closed before anything is proxied.
- `$limit_req_status` reports what happened to the connection, which is
  what makes `limit_req_dry_run` readable from an access log.
- A connection waiting on its delay is counted once, not once per event
  that walks the phases again while it waits.
- A build without `--with-stream` is rejected at configure time instead of
  failing at the link step.
