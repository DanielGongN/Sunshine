# Direct Video Source Address Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow direct video UDP and gateway-forwarded video UDP to coexist without using a loopback control address as the source for direct video packets.

**Architecture:** Store a source address in the per-session video state. Select loopback for a loopback gateway peer, otherwise probe the OS route to the established video peer once and use the resulting local interface address for both batched and unbatched video sends, with an address-family-compatible unspecified fallback.

**Tech Stack:** C++23, Boost.Asio UDP sockets, GoogleTest, CMake/Ninja, MSYS2 UCRT64

**Spec:** `docs/superpowers/specs/2026-09-10-direct-video-source-address-design.md`

## Global Constraints

- Run build and test commands through `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
- Use build directories prefixed with `cmake-build-`.
- Run the GoogleTest executable at `<build-directory>/tests/test_sunshine`.
- Format C/C++ changes according to `.clang-format`.
- Preserve both gateway-forwarded video and direct video modes.
- Do not change audio or control routing.

---

### Task 1: Add deterministic video source-address selection policy

**Files:**
- Modify: `src/stream.h:7-43`
- Modify: `src/stream.cpp:96-113`
- Test: `tests/unit/test_stream.cpp:1-110`

**Interfaces:**
- Consumes: video peer address plus an optional source address returned by route probing.
- Produces: `stream::detail::select_video_source_address(const boost::asio::ip::address &, const std::optional<boost::asio::ip::address> &) -> boost::asio::ip::address`.

- [ ] **Step 1: Write failing source-selection tests**

Add the required Boost.Asio and optional declarations to `tests/unit/test_stream.cpp`, then add these focused cases:

```cpp
TEST(VideoSourceAddressTests, KeepsLoopbackForGatewayPeer) {
  auto peer = boost::asio::ip::make_address("127.0.0.1");
  auto routed = boost::asio::ip::make_address("192.168.1.10");

  EXPECT_EQ(stream::detail::select_video_source_address(peer, routed), peer);
}

TEST(VideoSourceAddressTests, UsesResolvedIpv4AddressForDirectPeer) {
  auto peer = boost::asio::ip::make_address("223.85.203.217");
  auto routed = boost::asio::ip::make_address("192.168.1.10");

  EXPECT_EQ(stream::detail::select_video_source_address(peer, routed), routed);
}

TEST(VideoSourceAddressTests, UsesResolvedIpv6AddressForDirectPeer) {
  auto peer = boost::asio::ip::make_address("2001:db8::20");
  auto routed = boost::asio::ip::make_address("2001:db8::10");

  EXPECT_EQ(stream::detail::select_video_source_address(peer, routed), routed);
}

TEST(VideoSourceAddressTests, FallsBackToIpv4AnyWhenRouteIsMissing) {
  auto peer = boost::asio::ip::make_address("223.85.203.217");

  EXPECT_EQ(
    stream::detail::select_video_source_address(peer, std::nullopt),
    boost::asio::ip::address_v4::any()
  );
}

TEST(VideoSourceAddressTests, FallsBackToIpv6AnyForWrongAddressFamily) {
  auto peer = boost::asio::ip::make_address("2001:db8::20");
  auto routed = boost::asio::ip::make_address("192.168.1.10");

  EXPECT_EQ(
    stream::detail::select_video_source_address(peer, routed),
    boost::asio::ip::address_v6::any()
  );
}
```

- [ ] **Step 2: Build the focused test to verify it fails**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2"
```

Expected: compilation or linking fails because `select_video_source_address()` is not declared or defined.

- [ ] **Step 3: Declare and implement the selection policy**

Add `<optional>` to `src/stream.h` and declare:

```cpp
boost::asio::ip::address select_video_source_address(
  const boost::asio::ip::address &peer_address,
  const std::optional<boost::asio::ip::address> &routed_source_address
);
```

Implement it in `stream::detail`:

```cpp
boost::asio::ip::address select_video_source_address(
  const boost::asio::ip::address &peer_address,
  const std::optional<boost::asio::ip::address> &routed_source_address
) {
  if (peer_address.is_loopback()) {
    return peer_address;
  }

  if (routed_source_address &&
      routed_source_address->is_v4() == peer_address.is_v4() &&
      !routed_source_address->is_loopback() &&
      !routed_source_address->is_unspecified()) {
    return *routed_source_address;
  }

  return peer_address.is_v4() ?
           boost::asio::ip::address {boost::asio::ip::address_v4::any()} :
           boost::asio::ip::address {boost::asio::ip::address_v6::any()};
}
```

- [ ] **Step 4: Format the changed C++ files**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "clang-format -i src/stream.h src/stream.cpp tests/unit/test_stream.cpp"
```

- [ ] **Step 5: Build and run the focused tests**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake --build cmake-build-codex-tests --target test_sunshine -j2 && ./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_filter=VideoSourceAddressTests.*"
```

Expected: all `VideoSourceAddressTests` pass.

- [ ] **Step 6: Commit the policy and tests**

```bash
git add src/stream.h src/stream.cpp tests/unit/test_stream.cpp
git commit -m "test: cover video source address selection"
```

### Task 2: Route video through its selected per-session source address

**Files:**
- Modify: `src/stream.cpp:450-480`
- Modify: `src/stream.cpp:1646-1960`
- Modify: `src/stream.cpp:2292-2321`

**Interfaces:**
- Consumes: `stream::detail::select_video_source_address()` from Task 1 and the endpoint established by `recv_ping()`.
- Produces: `session_t::video.source_address`, used by every video send path.

- [ ] **Step 1: Add the per-video source address state**

Add the field alongside `session_t::video.peer`:

```cpp
boost::asio::ip::address source_address;
```

- [ ] **Step 2: Add one-time UDP route probing**

Add a private helper near the existing UDP endpoint utilities:

```cpp
boost::asio::ip::address resolve_video_source_address(const udp::endpoint &peer) {
  if (peer.address().is_loopback()) {
    return detail::select_video_source_address(peer.address(), peer.address());
  }

  asio::io_context route_ioc;
  udp::socket route_socket {route_ioc};
  boost::system::error_code ec;
  std::optional<boost::asio::ip::address> routed_source_address;

  route_socket.open(peer.protocol(), ec);
  if (!ec) {
    route_socket.connect(peer, ec);
  }
  if (!ec) {
    auto local_endpoint = route_socket.local_endpoint(ec);
    if (!ec) {
      routed_source_address = local_endpoint.address();
    }
  }

  auto source_address = detail::select_video_source_address(peer.address(), routed_source_address);
  if (source_address.is_unspecified()) {
    BOOST_LOG(warning) << "[stream][video-route] failed peer="sv << udp_endpoint_to_string(peer)
                       << " error="sv << (ec ? ec.message() : "invalid routed source")
                       << " fallback="sv << source_address.to_string();
  } else {
    BOOST_LOG(info) << "[stream][video-route] peer="sv << udp_endpoint_to_string(peer)
                    << " source="sv << source_address.to_string();
  }

  return source_address;
}
```

- [ ] **Step 3: Select the source after finalizing gateway/direct peer routing**

In `videoThread()`, retain the existing gateway peer rewrite and then assign:

```cpp
session->video.source_address = resolve_video_source_address(session->video.peer);
```

This must execute after the optional rewrite to `127.0.0.1:<video-port + UDP_FWD_OFFSET>` and before enabling QoS or starting capture.

- [ ] **Step 4: Use the video-specific source in both send paths**

Replace the two video-only `session->localAddress` arguments in `videoBroadcastThread()` with:

```cpp
session->video.source_address
```

Do not change the audio send paths, which continue using `session->localAddress`.

- [ ] **Step 5: Format and build**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "clang-format -i src/stream.cpp && cmake --build cmake-build-codex-tests --target test_sunshine -j2"
```

Expected: `test_sunshine` builds successfully.

- [ ] **Step 6: Run the full test suite**

Run:

```powershell
E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "./cmake-build-codex-tests/tests/test_sunshine.exe"
```

Expected: all tests pass.

- [ ] **Step 7: Inspect the final diff and commit**

Run `git diff --check` and verify that only video source selection changed. Then commit:

```bash
git add src/stream.cpp
git commit -m "fix: select routable source for direct video"
```

### Task 3: Validate both live transport modes

**Files:**
- No source changes expected.

**Interfaces:**
- Consumes: the completed binary and existing gateway/direct clients.
- Produces: runtime evidence that both routes select the correct source address.

- [ ] **Step 1: Validate gateway-forwarded video**

Connect with the existing gateway video mode and verify one log entry equivalent to:

```text
[stream][video-route] peer=127.0.0.1:<video-forward-port> source=127.0.0.1
```

Expected: video is delivered through the gateway and no routing regression occurs.

- [ ] **Step 2: Validate direct public video**

Connect control/audio through the gateway and send the raw video ping to Sunshine's public video UDP mapping.

Expected log shape:

```text
[stream][video-route] peer=<public-client-ip>:<client-port> source=<host-lan-ip>
```

Expected: the client receives video and the Sunshine log contains no `WSASendMsg() failed: 10051` entries.

- [ ] **Step 3: Record any environment-only validation limitation**

If either live client mode is unavailable in the development environment, report the exact unexecuted scenario; do not replace it with a claim based only on unit tests.
