# Audio Source Address Implementation Plan

> **For agentic workers:** Execute this focused repair inline in the existing checkout. The user approved the proposed source-address fix with “开始修复”. Preserve unrelated local changes.

**Goal:** Send gateway audio successfully even when audio PING arrives before the control connection initializes its local address.

**Architecture:** Share the existing video route resolver between video and audio. Resolve audio from its own UDP socket and final destination, then retain that independent source in the existing synchronized route.

**Tech Stack:** C++23, Boost.Asio, Windows Winsock, gtest, CMake/Ninja, MSYS2 UCRT64.

**Spec:** `docs/superpowers/specs/2026-10-20-audio-source-address-design.md`

## Global Constraints

- Audio source selection must not read the control connection's local address.
- Preserve IPv4/IPv6, multi-homed routing, optional audio startup, gateway framing, encryption, FEC, and media port rebinding.
- Use MSYS2 UCRT64, `cmake-build-*` directories, gtest, and the repository `.clang-format`.
- Preserve unrelated working changes and do not edit localization.

## Task 1: Independent audio source selection and regression coverage

**Files:**
- Modify `src/stream.cpp`: generalize the resolver, log both media sources, and resolve audio before retaining its route.
- Modify `src/stream.h`: rename the pure source selector declaration for shared media use.
- Modify `tests/unit/test_stream.cpp`: migrate existing video selector coverage and add route resolution and Windows send regressions.

**Interfaces:**

```cpp
boost::asio::ip::address select_media_source_address(
  const boost::asio::ip::address &peer_address,
  const std::optional<boost::asio::ip::address> &routed_source_address
);

boost::asio::ip::address resolve_media_source_address(
  const boost::asio::ip::udp::endpoint &peer,
  const boost::asio::ip::address &bound_address,
  std::string_view stream_type
);
```

- [ ] Extend the existing selector tests with IPv4/IPv6 loopback without a routed source. Test the resolver with a wildcard IPv4 gateway binding, an IPv6 loopback destination, and a concrete direct source.
- [ ] Add a Windows send regression using two ephemeral IPv4 UDP sockets, `resolve_media_source_address(receiver.local_endpoint(), sender.local_endpoint().address(), "audio")`, and `platf::send`. Assert the resolved source equals the receiver's loopback address and the send API returns true without creating a control connection.
- [ ] Generalize the source selector/resolver. Keep the loopback selection and existing direct-route fallback behavior, and log `[stream][audio-route]` or `[stream][video-route]` for every resolved route.
- [ ] Replace audio's retained `session->localAddress` with its independently resolved source. Read the audio socket's binding with an error-code overload and use a matching-family wildcard if that lookup fails, mirroring video.
- [ ] Format, build, and run the focused regression suite with the repository MSYS2 shell prefix:

```sh
/e/WorkComp/LLVM/bin/clang-format.exe -i src/stream.cpp src/stream.h tests/unit/test_stream.cpp
cmake --build cmake-build-codex-tests --target test_sunshine sunshine -j 2
./cmake-build-codex-tests/tests/test_sunshine.exe --gtest_filter=MediaSourceAddressTests.*:MediaSourceResolutionTests.*:StreamRouteTests.*:StreamPingTests.*:StreamTimeoutNotificationTests.* --gtest_output=xml:cmake-build-codex-tests/audio-source-focused-tests.xml
```

- [ ] Run the full suite once, record results, inspect the diff and formatting, then commit only this repair with a Chinese `fix:` message.
