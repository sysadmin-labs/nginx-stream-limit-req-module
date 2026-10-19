# Changelog

Newest first.  Dates are release dates.

## v0.1.0 (2026-10-19)

### Added

- First release.  `limit_req_zone` and `limit_req` for the stream
  subsystem, with `burst`, `nodelay`, `delay=`, `limit_req_log_level` and
  `limit_req_dry_run` -- the same spelling and the same leaky bucket as the
  http directives of those names.

- The handler runs in the preaccess phase, so a refused connection is closed
  before anything is proxied.

- A build without `--with-stream` is refused at configure time, with a
  sentence that names the missing option, instead of failing much later at
  the link step.

### Why this module exists

nginx limits how many stream connections are open at once (`limit_conn`)
but not how fast they arrive; `limit_req` exists only for http.  The gap is
a standing request in the nginx community forum, and nothing on GitHub
fills it.
