# Baseline Definition

The baseline fixes the reference implementations and the minimum checks needed before libretro integration. It provides a repeatable comparison point for the [development roadmap](roadmap.md).

## Reference revisions

| Project | Role | Fixed revision |
| --- | --- | --- |
| [PCSX2x6](https://github.com/PS2Homebrew-arcade/pcsx2x6) | Initial emulation base and standalone arcade reference | `583f0e90907d69bb2db670f570d0865260fcc71b` |
| [PCEE2](https://github.com/WizzardSK/pcee2-libretro) | Primary libretro integration reference | `5719627731beb90e379ff1be1223391b7f563b3b` |
| [PCSX2](https://github.com/PCSX2/pcsx2) | Upstream behavior and interface reference | `1f67c53b6723fcd6a90041a5b296f0e34a6a3222` |
| [paraLLEl-PS2](https://github.com/libretro/parallel-ps2) | Scheduling and rendering reference, primarily for M4 | `c8b51cfe0f63d87c58dae735dc64285e79fde76e` |

PCEE2 records upstream PCSX2 `v2.9.138` at `ab0b401f99729ef8da3009f9af28d8ee96e0e7ba` in its `upstream.version`. Integration must account for differences between that upstream revision and the selected x6 interfaces.

M0 requires source builds of PCSX2x6 and PCEE2. PCSX2 and paraLLEl-PS2 serve as source references; building them is not an M0 requirement.

## Initial scope

The initial target is desktop Windows x86-64. Reference builds retain their own dependency requirements and runtime resources. The PCEE2 check uses a fixed RetroArch version and one graphics backend.

The initial System246 integration target is NM00002, Bloody Roar 3: a button-controlled CD title. Its `.acgame`, BIOS, boot ELF, dongle, media, JVS layout, GameDB settings, and required patches form one complete reference case. System256, HDD media, and other input layouts are separate coverage cases.

## Independent reference checks

| Reference | Content and environment | Required check | Purpose |
| --- | --- | --- | --- |
| PCSX2x6 | Standalone build with an arcade `.acgame` and its required resources | Startup checkpoint, required input, normal shutdown and persistence | Establish the arcade behavior used for comparison |
| PCEE2 | Source-built core in the selected RetroArch version, with ordinary PS2 content or suitable homebrew | Core load, guest startup, emulation progress, normal shutdown and core unload | Establish the libretro integration reference |

PCEE2 is not expected to implement x6's `.acgame` boot path. Each reference is checked with content appropriate to its existing capabilities.

## M0 acceptance

- Both selected reference revisions build from source without emulation changes introduced for the check.
- Required dependencies, runtime resources, and relevant configuration are identifiable and repeatable.
- PCSX2x6 reaches an agreed arcade checkpoint and completes normal shutdown, including supported writable-state persistence.
- PCEE2 loads in the frontend, starts its reference content, advances emulation, and completes normal shutdown and core unload.
- Each check has a short stopping point and a defined validation scope.

M0 establishes runnable build and lifecycle references. Rendering correctness, audio quality, gameplay input in the new core, and reset/reload behavior receive their own acceptance criteria in M1–M3. Performance and broad compatibility are later milestones.

## Comparison parameters

The following identities define a repeatable reference case:

| Parameter | Purpose |
| --- | --- |
| Project and source revision | Identify the implementation being compared |
| Build and dependency profile | Fix the relevant compilation and runtime-library choices |
| Frontend version and backend settings | Fix the libretro host configuration |
| Content identity and board variant | Distinguish game revisions and hardware modes |
| BIOS, ELF, dongle, media and patch identities | Define the complete arcade boot inputs where applicable |
| Relevant configuration | Fix renderer, clock/memory settings, JVS layout, audio and writable-state handling |
| Checkpoint and actions | Define a bounded comparison with a clear stopping point |

## Updating the baseline

Reference revisions remain fixed during a milestone. An update identifies the previous and new revisions, the reason for the change, affected interfaces or behavior, and the focused checks needed to retain a useful comparison point. A successful check of one board, game, or backend does not establish coverage for other cases.
