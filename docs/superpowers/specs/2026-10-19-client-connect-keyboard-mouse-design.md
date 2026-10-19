# Keyboard and mouse initialization on client_connect

The requested change moves Windows keyboard/mouse backend loading from service
startup to a successfully authorized upstream `client_connect`. Initialize it
before the event's `operate_result` acknowledgment and independently of the
`middleware_gamepad_preinit` setting.

Keep the process-wide platform input context, ViGEm probe, and touch/pen setup at
startup. When middleware is enabled, leave its keyboard/mouse backend unselected
until the event. For standalone operation without middleware, initialize it
during the existing input startup sequence.

Expose `input::preinitialize_keyboard_mouse()` to submit the platform operation
to the existing single-worker input task pool and wait for completion. Keep
backend selection and cursor-mode logging on that worker, ordered with streamed
input. Existing shutdown ordering already joins middleware before the worker.

Selection retains the existing priority: try `interception.dll` beside the
Sunshine executable, requiring a valid DLL, exports, driver context, and keyboard
and mouse devices; otherwise use SendInput. The current backend implementation
has no Buke fallback and this change does not add one.

Initialization is idempotent for the lifetime of the input context, including
the SendInput fallback. Repeated authorization events keep the selected backend
so a press and release cannot switch input devices midstream. Invalid UUIDs,
missing certificates, and rejected certificates do not initialize input.
Initialization does not change the authorization meaning of the existing
acknowledgment status, matching the existing gamepad preinitialization behavior.

Validate explicit initialization, Interception priority, stable fallback, and
repeated calls using the existing fake DLL. Run the affected input and middleware
gtests under MSYS2 UCRT64, format changed C++ lines with `.clang-format`, and
commit only this task with a Chinese `feature:` message and numbered details.
Real driver behavior still requires a client and the installed driver.
