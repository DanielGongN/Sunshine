# Local timeout disconnect design

The user approved changing both local timeout paths from reminders into immediate stream termination. Sunshine must send `0x8004E001` for force-hold timeout or `0x8004E002` for idle timeout, stop that session, and report `disconnected` with `data.type=1/2` and a concrete message.

Use the existing control-thread termination sender followed by `session::stop`. Set the session's termination code before sending so shutdown retries retain the same code. Let `session::stop` report the matching timeout reason through the existing first-cause and middleware notification deduplication paths. Do not wait for a platform reply. The existing timeout thresholds, force-hold precedence, disabled settings, and control-connection prerequisite remain applicable.

Two alternatives were considered: remembering a reminder until an upstream reply still depends on platform availability; routing local expiry through the global upstream-disconnect queue could stop unrelated sessions and adds a round trip through another iteration. Direct control-thread termination affects only the expired session and satisfies the requested behavior.

Later upstream replies only act on running sessions, so they must not overwrite the selected timeout code or repeat a completed local stop. A client send failure must not prevent session shutdown or platform reporting; the existing shutdown path retries the selected code. Reporting remains subject to the existing middleware availability and once-per-client-cycle rules.

Validation uses the existing timeout selection, disconnect first-cause, middleware report/cycle, and upstream-code tests. Review the control flow for send-before-stop, retained retry code, and no dependency on platform acknowledgement; compile both test and service targets with MSYS2 UCRT64. Update the current protocol documentation to describe local timeout termination. Real Android packet delivery requires a connected client and is outside the automated test environment.
