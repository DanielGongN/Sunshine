# Input and Gateway Control Latency Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Keep keyboard and mouse control latency low under high-frequency mouse input while preserving the existing Gateway UDP frame format and direct-video transport.

**Architecture:** First make the Sunshine input drain observable and latency-aware, merging only safe relative mouse motion while never crossing keyboard, button, or other control boundaries. Then remove the Gateway's serialized Control user-space queues and isolate UDP dispatch from TCP work, with bounded in-flight sends and aggregate latency metrics.

**Tech Stack:** C++20, Boost.Asio, GoogleTest, MSYS2/UCRT64, existing Sunshine task pool and Windows input backends.

**Spec:** `docs/superpowers/specs/2026-09-20-gateway-control-fast-path-design.md`

## Global Constraints

- Preserve the public Gateway UDP frame `[stream_id][length][payload]` exactly.
- Keep Control, audio, and client microphone traffic on the Gateway public UDP port.
- Keep direct video as the normal video path and retain the Gateway video compatibility path.
- Keep ENet payloads and Windows input APIs compatible.
- Build with the MSYS2 UCRT64 command prefix required by `AGENTS.md`.
- Use a `cmake-build-*` build directory and run `tests/test_sunshine`.

---

### Task 1: Add input-queue latency and batching test seams

**Files:**
- Modify: `src/input.cpp`
- Modify: `src/input.h`
- Modify: `tests/unit/test_input.cpp`

**Interfaces:**
- Produce a small testable decision helper for classifying adjacent input packets and calculating safe relative-motion batches.
- Preserve existing packet structures and public input entry points.

- [x] **Step 1: Write failing tests** for relative mouse accumulation without 16-bit overflow.
- [x] **Step 2: Run the focused GoogleTest target and confirm the new tests fail for the missing helper/behavior.
- [x] **Step 3: Implement the minimal helper and wire it into the existing input drain.
- [x] **Step 4: Run the focused tests and confirm they pass.

### Task 2: Make high-frequency mouse draining latency-aware

**Files:**
- Modify: `src/input.cpp`
- Modify: `src/input.h`
- Modify: `tests/unit/test_input.cpp`

**Interfaces:**
- Keep keyboard, mouse button, scroll, and gamepad ordering barriers intact.
- Keep existing queue peak statistics and avoid per-packet logging while enabling safe motion batching.

- [x] **Step 1: Add a failing test for the relative mouse batching decision used by the drain.
- [x] **Step 2: Run the focused test and verify it fails before production changes.
- [x] **Step 3: Implement bounded mouse coalescing and retain the existing single active drain-task scheduling.
- [x] **Step 4: Run all input unit tests and verify no regression in gamepad and keyboard dispatch.

### Task 3: Remove Gateway Control user-space serialization

**Files:**
- Modify: `src/gateway.cpp`
- Modify: `src/gateway.h`
- Modify: `tests/unit/test_gateway.cpp`

**Interfaces:**
- Preserve all stream IDs and the three-byte UDP framing header.
- Submit Control sends directly from the UDP receive callbacks with a bounded in-flight limit and generation checks.

- [x] **Step 1: Add failing tests for existing control frame length parsing and in-flight accounting.
- [x] **Step 2: Run the focused Gateway tests and verify the expected failures.
- [x] **Step 3: Implement direct Control send paths, completion accounting, and stale-endpoint rejection while leaving audio/video queue policies unchanged.
- [x] **Step 4: Run Gateway unit and integration tests.

### Task 4: Isolate UDP execution and add aggregate metrics

**Files:**
- Modify: `src/gateway.cpp`
- Modify: `tests/unit/test_gateway.cpp`
- Modify: `docs/superpowers/specs/2026-09-20-gateway-control-fast-path-design.md`

**Interfaces:**
- Use separate TCP and UDP `asio::io_context` workers.
- Emit one aggregate Control metric record per active second with submit/complete maximum latency and drops/errors.

- [ ] **Step 1: Add failing tests for metric bucket updates and percentile calculation.
- [ ] **Step 2: Run the focused tests and verify failure.
- [x] **Step 3: Implement the UDP worker split, metrics, shutdown synchronization, and rate-limited overload diagnostics.
- [x] **Step 4: Build with MSYS2/UCRT64 and run `cmake-build-*/tests/test_sunshine`.
- [x] **Step 5: Inspect the diff and record any environment-dependent performance limitations.
