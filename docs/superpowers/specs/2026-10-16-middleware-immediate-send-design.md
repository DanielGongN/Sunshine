# Immediate middleware event delivery

The connected event must start writing as soon as a stream session queues it,
without waiting for incoming upstream traffic or the 30-second heartbeat.
An unacknowledged connected event must still retry every 30 seconds with the
same inner message ID. A matching consume_record_create_success event with
data.code equal to zero cancels retries.

Use one asynchronous WebSocket writer on the existing middleware io_context.
Start it whenever a message is queued and continue from each write completion.
Keep each message buffer alive until its write completes. Only one application
write may be active, while the existing asynchronous read remains active.
Route heartbeat and read-completion queue draining through the same writer.
Subscription writes finish before the asynchronous message writer starts.

Removing the read guard while retaining synchronous writes would mix synchronous
writes with the asynchronous read's control-frame writes. A separate writer
thread would add synchronization and lifecycle work. The single io_context
writer fits the current architecture and coordinates reads and writes through
Boost.Beast's asynchronous interface.

Validate with a local WebSocket upstream that sends no messages after its
handshake: connected must arrive within two seconds, queued messages must
retain order, and connected must retry after 30 seconds until acknowledged.
Build with MSYS2 UCRT64, use a cmake-build- directory, and follow .clang-format.
