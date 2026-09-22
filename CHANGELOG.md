# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.1] - 2026-09-21

### Changed

- The daemon now builds against protothread 2.0.0. The vendored `protothread.h`
  was updated to the header-only, freestanding v2 release, and the daemon's
  scheduler handle is now a `protothread_t` (the v2 name; `state_t` was removed).
  moon-flare uses only the documented protothread API, so no other call sites
  changed.

### Fixed

- The TUI HTTP client now times out a stalled in-flight request after 8 seconds
  and retries with backoff, instead of waiting indefinitely when the daemon
  accepts a connection but never answers.

## [0.1.0]

Initial daemon, REST API, plugins, CLI, and TUI.
