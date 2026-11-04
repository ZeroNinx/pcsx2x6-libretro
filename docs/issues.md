# Known Issues

Open compatibility issues include emulator defects and frontend integration limitations. Their scope and follow-up are listed below; a working input mapping or completed reset request does not establish full input or emulator-reset correctness.

| ID | Issue | Status | Follow-up |
| --- | --- | --- | --- |
| ISSUE-001 | Missed drum hits in fast charts | Reported in both the core and standalone PCSX2x6; cause unresolved | Deferred behind performance work; input comparison with paraLLEl-PS2 |
| ISSUE-002 | Reset failure after arcade execution | Also reproducible in standalone PCSX2x6; upstream defect unresolved | Separate emulator reset investigation |
| ISSUE-003 | Excess audio after resetting a running VM | Observed in the core; suspected SPU2 timing cause unverified | Investigate with ISSUE-002 |
| ISSUE-004 | Switching fullscreen ends the content session | Same-device video-driver recreation is supported; device or queue changes still end content | Remaining context and reload regressions before baseline replacement |

## ISSUE-001: Missed drum hits in fast charts

NM00054 can miss drum input during fast charts. Similar behavior has been reported in standalone PCSX2x6. Basic two-player Don/Ka mapping and ordinary press/release behavior work, but high-speed accuracy is not established. There is no confirmed cause or automated reproduction yet.

Follow-up will compare frontend sampling, input latching, frame progression and JVS reads with standalone PCSX2x6 and paraLLEl-PS2. Establish a repeatable chart/device case before selecting a fix; a scheduling change is not assumed to solve it.

## ISSUE-002: Reset failure after arcade execution

Resetting NM00002 after it has been running can fail during the subsequent boot, with TLB misses and the assertion `FQC = 0 on VIF FIFO READ!`. Both running and paused VM reset paths are affected, and standalone PCSX2x6 can reproduce the failure. Reset before the first frame has not shown the same failure.

**Workaround:** close the content and load it again when restarting a running title. The core preserves the emulator's reset behavior; it does not substitute content reload or suppress assertions.

The core's CPU-thread dispatch, refusal handling, completion acknowledgement and output/input cleanup are separate from the emulator's ability to continue executing after reset. The underlying failure remains unresolved. Differences between initial arcade-device setup and reset are an investigation lead, not a confirmed cause. Any emulator fix should remain separate from libretro integration.

## ISSUE-003: Excess audio after resetting a running VM

After a VM has executed, its first frame following reset can contain more audio than a normal frame. The extra samples are generated after reset rather than retained from before it. Reset before the first `retro_run` has instead produced no audio on the following first frame.

A possible cause is SPU2 clock state: `SPU2::Reset()` does not synchronize `lClocks` with the IOP cycle counter as `SPU2::Open()` does. This may trigger catch-up mixing after the IOP counter resets. That explanation is based on source analysis and has not been confirmed by tracing the clocks; the matching reset code in PCSX2 does not establish an independently reproduced PCSX2 bug.

The excess remains within the bounded frontend queue. Its audibility and sample contents are not established. Investigate it with ISSUE-002; the libretro layer does not trim or compensate for it as a workaround.

## ISSUE-004: Switching fullscreen ends the content session

The direct Vulkan-image path requests context caching and refreshes the frontend render interface at frame boundaries. When a video-driver recreation preserves the device and graphics queue, the VM continues and the latest valid image is handed to the new driver, including while the VM is paused.

If the context is destroyed or the replacement interface cannot use the GS device and queue, the core ends the session and reports the failure once. Recovery onto a different device or queue is unsupported. The remaining context and close/reopen regressions must pass before the hardware-video implementation replaces the functional baseline.
