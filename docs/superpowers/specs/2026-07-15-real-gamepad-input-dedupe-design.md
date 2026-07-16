# Real Gamepad Input Dedupe Design

## Goal

Optimize the real controller input path so button press/release edges remain reliable while repeated identical controller states are not sent repeatedly to the virtual gamepad backend.

This change explicitly excludes the middleware `click_gamepad` path.

## Design

- Treat real controller packets as state updates, but preserve button edges by never batching or dropping packets where `buttonFlags` changes.
- Drop a real controller update before platform dispatch when the full normalized state matches the last state stored for that controller.
- Keep existing batching behavior for non-button state changes, where repeated stick or trigger movement can collapse to the latest state.
- Add a Windows ViGEm backend guard that skips identical X360 reports. DS4 state updates keep the existing timestamp refresh behavior, but repeated controller state should not be treated as a new real input edge.

## Verification

- Confirm that `A down -> A up` still dispatches both edges.
- Confirm that repeated identical controller packets do not repeatedly call into the platform backend.
- Confirm that button changes stop batching while stick/trigger-only updates may still batch.
- Build and run the available test executable when practical.
