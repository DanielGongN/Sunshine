# Audio source address independent of control startup

## Problem and requirements

The reported session receives its audio PING at 15:05:12.668, before the control connection initializes `session->localAddress` at 15:05:12.933. Audio copies the default IPv4 unspecified address into its retained route. Windows then rejects audio sends to the gateway loopback endpoint with `WSAEINVAL` (10022). The retained source never changes when control connects.

- Audio source selection must not read the control connection's local address.
- A gateway destination on `127.0.0.1` must select `127.0.0.1` as its source, including when audio starts before control.
- Direct audio must retain IPv4/IPv6 and multi-homed routing behavior by resolving against its own UDP socket and destination.
- Preserve optional audio startup, gateway framing, encryption, FEC, and media port rebinding.
- Log the selected audio destination and source, including loopback routes.
- Use MSYS2 UCRT64, `cmake-build-*` directories, gtest, and the repository `.clang-format`. Preserve unrelated working changes and do not edit localization.

## Approach

Generalize the existing video source selector/resolver for both media streams. Each worker supplies its own socket's bound address and its final destination after gateway rewriting. Loopback destinations select the corresponding loopback source. A concrete bound address is retained for a direct destination; a wildcard binding resolves the outgoing address with a temporary UDP socket. Existing matching-family wildcard fallback remains when route discovery fails.

Reuse the existing resolution logic instead of waiting for the control connection, which would couple optional audio startup to another worker. Merely forcing a loopback source for all audio would break direct streaming, so source selection follows the actual destination.

The audio worker resolves and retains its source before capture, without accessing `session->localAddress`. Both workers use stream-specific route logs. The retained destination remains synchronized by the existing `media_route_t` implementation.

## Validation

Extend source selection coverage for missing control state and IPv6 loopback. Exercise the shared resolver for wildcard gateway bindings, an explicitly bound direct route, and IPv6. Add a Windows regression that uses the resolved gateway source with the real `platf::send` path before any control connection exists; assert the API accepts the datagram. Build `sunshine` and `test_sunshine` with UCRT64, run the source/route/PING regression suites, and run the complete suite once to identify unrelated failures. Actual client playback requires a separate streaming session.
