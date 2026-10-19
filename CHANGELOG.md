# Changelog

## v0.1.0 (2026-10-19)

- First release: `limit_req_zone` and `limit_req` for the stream subsystem,
  with `burst`, `nodelay`, `delay=`, `limit_req_log_level` and
  `limit_req_dry_run`, spelled and counted as in the http module.
- The handler runs in the preaccess phase, so a refused connection is
  closed before anything is proxied.
- A build without `--with-stream` is rejected at configure time instead of
  failing at the link step.
