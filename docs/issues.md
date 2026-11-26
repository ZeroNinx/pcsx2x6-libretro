# Known Issues

| ID | Issue | Status |
| --- | --- | --- |
| ISSUE-001 | Missed drum hits in fast charts | Held-input overlap fixed; other timing limits remain |
| ISSUE-002 | Reset failure after arcade execution | Open |
| ISSUE-003 | Excess audio after resetting a running VM | Open |
| ISSUE-004 | Video-driver recreation on another device or queue ends the content session | Cross-device recovery unsupported |
| ISSUE-005 | Higher CPU power with RTSS frame limiting | Workaround available |
| ISSUE-007 | Unreproduced exit crash after a save-state session | Not reproduced |

## ISSUE-001: Missed drum hits in fast charts

Each digital drum press now produces a one-guest-frame pulse. This fixes later hits being suppressed by overlapping held buttons. Holding a button does not repeat the hit.

Different drum parts struck in the same frame still follow the game's input rules. A short press that the frontend does not sample cannot be recovered by the core.

## ISSUE-002: Reset failure after arcade execution

Resetting NM00002 after gameplay can fail during reboot. Both running and paused resets are affected, and the problem also occurs in standalone PCSX2x6.

**Workaround:** close the content and load it again instead of using reset.

## ISSUE-003: Excess audio after resetting a running VM

The first frame after reset can generate extra audio. The cause and audible effect remain unconfirmed. Use the close-and-reload workaround for ISSUE-002.

## ISSUE-004: Video-driver recreation on another device or queue ends the content session

Normal fullscreen switching works when the frontend keeps the graphics device. Switching to another device ends the content session; Vulkan also requires the same graphics queue. Reload the content after such a change.

## ISSUE-005: Higher CPU power with RTSS frame limiting

On some handheld systems, RTSS frame limiting at 60 FPS can increase CPU power while the game remains at normal speed. RTSS adds CPU work in the frontend's presentation path; its interaction with processor power management is not fully understood.

**Workaround:** set the RTSS frame limit for RetroArch to **0**. RetroArch's video and audio synchronization provide pacing. RTSS can remain running for its overlay.

## ISSUE-007: Unreproduced exit crash after a save-state session

An exit crash after save-state operations in NM00054 has been reported but not reproduced. If it recurs, retain the session log and note the exit action and renderer.

## Frontend pause and video-driver recreation

In RetroArch 1.22.2, changing display mode while paused can leave a black screen until the content resumes. This affects both Vulkan and Direct3D 11.

**Workaround:** resume the content after changing display mode.
