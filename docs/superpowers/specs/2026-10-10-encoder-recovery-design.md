# Encoder recovery and upstream video fixes

Approved in chat on 2026-10-10 after comparing remote `p` (`LizardByte/Sunshine`, master `3411311a`) with the current branch. The user requested implementation of the useful fixes.

## Behavior

- Restore the upstream asynchronous NVENC recovery path. A failed encoding run returns its error to the outer capture loop instead of immediately recording a fatal disconnect.
- Permit at most three encoder rebuilds per streaming session, including failures while initializing a replacement encoder. The fourth encoder failure terminates the session with type 41 and the final error message. The budget is not replenished by individual successful frames, so persistent driver hangs cannot create unlimited detached teardown threads.
- Apply recovery only to encoders supporting asynchronous teardown. Capture failures and failures on other encoders remain terminal. Explicit shutdown and capture-worker termination take precedence over retrying.
- A recoverable failure does not enter the first-cause disconnect record and does not notify middleware. Preserve first-cause tracking and notification deduplication for terminal failures.
- Keep the session, control channel, and frame sequence alive during reconstruction. Force an IDR at the start of each replacement encoder session and log each rebuild attempt. Keep the existing 100 ms NVENC completion wait.

## Upstream fixes

- `7ecd0286` / #5346: check shutdown, image-worker termination, and capture reinitialization immediately before encoding.
- `e40d355f` / #5249: drain images through an atomic, nonblocking `event_t::try_pop()` instead of a racy `peek()` followed by blocking `pop()`. Preserve the existing working-tree changes to `thread_safe.h`.
- `86a25385` / #5257: flush an AVCodec session during destruction only when it has encoded frames; also tolerate an empty context.
- `9051ae84` / #5554: AV1 probing must update AV1 constraints and inspect AV1 capabilities, without modifying HEVC results.

The dynamic NVENC SDK selection in #5451 is a separate compatibility and dependency refactor. It does not change the timeout or recovery policy and is outside this focused fix.

## Validation

Use deterministic GTest coverage for transient failures preserving later stop reasons, three permitted rebuilds, terminal exhaustion retaining the final frame error, immediate capture/nonrecoverable errors, and existing first-cause ownership. Test nonblocking image draining for empty, consumed, stopped, and pointer-valued events. Build `sunshine` and `test_sunshine` with MSYS2 UCRT64 in `cmake-build-codex-tests`, then run the recovery/disconnect/route/control regressions. Follow `.clang-format` and preserve unrelated working-tree changes. Hardware NVENC timeout injection and end-to-end client playback require the streaming host/client environment.
