# Changelog

## v0.1.2 (2026-10-21)

### Fixed

- The gates that load the module named it without the stream subsystem it
  resolves `ngx_stream_core_module` out of, and the suite let a glob build
  the list, which sorts the submodule ahead of `ngx_stream_module.so`.
  Both failed the `dlopen` with an undefined symbol, so a loadable build
  went untested on Linux and on FreeBSD.  The module itself was never at
  fault, and whoever writes `load_module` lines by hand needs the same
  order.

## v0.1.1 (2026-10-21)

### Fixed

- The module refused a build whose stream subsystem is a loadable object.
  `--with-stream=dynamic` sets `$STREAM` to `DYNAMIC` and not to `YES`, and
  the check in `config` took only `YES`, so configure stopped with
  "requires --with-stream" on a build that had asked for exactly that.  It
  is how the FreeBSD nginx ports build, and it is what ci/build.sh asks for
  in its loadable mode now, so the build that found this is the one that
  watches it.

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
- The key is read in the preaccess phase, which the README now says, since a
  variable that is only known later leaves the zone applying to nothing.
- A build without `--with-stream` is rejected at configure time instead of
  failing at the link step.
