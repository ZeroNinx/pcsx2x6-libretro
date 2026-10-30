# Development Roadmap

Planning snapshot: 2026-10-30. This document defines milestone boundaries and acceptance criteria. Each milestone has its own progress and planning document. M1 contains the initial implementation breakdown; later milestones will be expanded when entered.

## Objective and initial scope

Build a libretro core that preserves PCSX2x6's NAMCO System 246 / 256 emulation behavior. Use PCSX2x6 as the initial emulation base, PCEE2 as the primary integration reference, PCSX2 as the upstream behavior reference, and paraLLEl-PS2 as a scheduling and rendering reference.

The initial implementation target is desktop Windows x86-64 with one fixed RetroArch version and one graphics presentation path. Additional platforms, graphics backends, and arcade peripherals are later work. There is no frame-rate or latency improvement target for the first functional core.

The emulation source keeps its upstream paths under `pcsx2/` and `common/`, with a separate `libretro/` frontend. Build support, runtime resources, and dependencies remain under `cmake/`, `bin/`, and `3rdparty/`. Imported code retains its copyright and license notices under the project's GPLv3 licensing.

## Milestones at a glance

| Milestone | Question answered | Main result |
| --- | --- | --- |
| [M0 — Establish baselines](M0.md) | Do the two reference builds work independently? | Reproducible PCSX2x6 and PCEE2 reference builds with bounded smoke checks |
| [M1 — Minimal libretro Host](M1.md) | Can the frontend load the library and manage its lifetime safely? | Shared-library target, Host adapter, VM lifecycle boundary, logging |
| [M2 — Boot one System246 game](M2.md) | Does the existing arcade boot path survive the new Host? | One System246 title reaches an agreed startup checkpoint |
| [M3 — Basic video, audio, and input](M3.md) | Does the playable core preserve standalone behavior? | One functional presentation path, audio delivery, JVS input, save-data handling |
| [M4 — Investigate scheduling](M4.md) | Which synchronization choices improve the measured workload? | Scheduling analysis and a measured decision for each proposed change |
| [M5 — Performance and compatibility](M5.md) | Which additional targets and workloads justify further work? | Separately scoped optimization and compatibility improvements |

M0 precedes integration. M1 establishes the Host boundary; M2 validates arcade boot; M3 establishes the functional reference for M4 and M5. Limited display or device plumbing may be implemented earlier when required by VM initialization or boot diagnosis.

## M0 — Establish baselines

**Purpose:** separate reference-project behavior from future porting failures.

Build unmodified PCSX2x6 and PCEE2 at fixed revisions. Exercise each in its intended environment: PCSX2x6 as a standalone arcade emulator, PCEE2 as a libretro core in the selected frontend. PCEE2's reference content is ordinary PS2 content or a suitable homebrew ELF, not an `.acgame` file.

The [baseline definition](baseline.md) fixes reference revisions, comparison parameters, and acceptance scope.

**Exit criteria:**

- Both source builds finish and their required runtime resources are identified.
- PCSX2x6 reaches a known arcade checkpoint, accepts the required input, and exits normally.
- PCEE2 loads in the selected frontend, starts its reference content, advances emulation and audio delivery, and unloads its content and core during a normal frontend shutdown without hanging.
- Each result identifies its revision, dependencies, frontend where applicable, settings, content identity, and limitations sufficiently to repeat the check.

M0 establishes runnable build and lifecycle references. Rendering correctness, audio quality, and the new core's interactive behavior have separate requirements in M1–M3. Reference behavior is evaluated within the scope of the specific check.

**Deferred:** port implementation, optimization, and broad game testing. The selected System246 case requires a standalone reference check before M2; other board and media cases provide separate coverage.

## M1 — Minimal libretro Host

**Purpose:** establish the smallest usable integration boundary before adapting arcade boot.

Scope includes a shared-library build, libretro exports and callback registration, configuration and resource initialization, frontend logging, and a Host adapter around VM initialization, CPU-thread execution, stopping, and cleanup. PCEE2 supplies the primary design reference; its code must be adapted to the selected x6 revision rather than assumed to be directly interchangeable.

VM lifetime follows x6's VMManager flow. M1 also repairs incomplete arcade startup rollback, pairs page-fault-handler ownership with VM-memory lifetime, and resolves the selected graphics path's runtime dependencies. These are focused correctness prerequisites, not scheduling optimizations. The CPU worker is created on first content load; the CPU thread owns VM state transitions. Minimum GS, audio, or device hooks required for real VM initialization belong here as necessary plumbing. A library that exports the ABI but only simulates VM state changes does not complete this milestone. The detailed contract is in [M1](M1.md).

**Exit criteria:**

- The frontend loads and unloads the library, with working initialization and deinitialization.
- At least one real VM initialization/shutdown cycle completes using a known baseline case or a suitable diagnostic boot path; a visible game checkpoint is not required yet. The current concrete case is the isolated NM00002 copy specified in [M1](M1.md).
- CPU-thread ownership, work dispatch, stop requests, and callback responsibilities are explicit.
- Preflight rejection and a controlled startup failure after media open both produce actionable logs, clean up their resources/state, and allow another attempt.
- The Host can stop during startup and release resources without exiting the frontend process.

**Deferred:** complete System246 boot coverage, full media delivery, gameplay, and performance work. This milestone establishes synchronization correctness; M4 does not postpone that requirement.

## M2 — Boot one System246 game

**Purpose:** preserve the arcade startup sequence through the libretro Host.

Select one System246 title verified in standalone x6 at the same revision. Prefer ordinary button input and a simple media arrangement. System256 and HDD content provide separate regression cases.

Scope includes `.acgame` recognition and metadata, BIOS selection, boot ELF and arguments, dongle/storage-card configuration, arcade media, SRAM, memory and clocks, IOP/DEV9 devices, game identity, GameDB fixes, and required patches. The original initialization and shutdown sequence remains the reference behavior.

**Exit criteria:**

- The selected `.acgame` reaches the same agreed startup checkpoint as standalone x6.
- Logs identify the intended game, board variant, BIOS, ELF, media, JVS mode, and applied patches.
- Minimal frame presentation is available to confirm the checkpoint when needed. This can be brought forward from M3; it does not imply complete audio/video support.
- Missing BIOS, ELF, dongle, or media is reported as a recoverable load failure.
- Writable data is separated from immutable reference inputs, and normal close performs the required device shutdown and persistence.

Writable-state handling includes SRAM/cards and BIOS-side `.nvm`/`.mec` files. Resolve the known SRAM read-handle and write-protection defects as described in [M2](M2.md), independently of libretro-specific path adaptation.

**Deferred:** additional board variants, multiple media arrangements, complete input layouts, and broad compatibility claims.

## M3 — Basic video, audio, and input

**Purpose:** establish a functional core whose observable behavior can be compared with standalone x6.

Use one presentation path, such as software rendering with framebuffer readback or the smallest practical bridge to an existing backend. Software rendering does not imply that presentation requires no graphics context. The path is chosen after the M0/PCEE2 investigation; zero-copy output and additional backends are not prerequisites.

Scope includes frame delivery and geometry, actual VM refresh rate, sample-rate reporting, bounded audio buffering, JVS button and coin input, Service/Test controls, reset, content reload, and persistent arcade data. The frontend controls overall pacing; the Host must coordinate VM progress with `retro_run()` rather than retain a competing desktop limiter. API responsibilities follow the [libretro core development documentation](https://docs.libretro.com/development/cores/developing-cores/).

**Exit criteria:**

- The selected System246 case produces usable video and audio and reaches the agreed short interactive checkpoint.
- Basic JVS controls match the standalone mapping; coin input uses press edges, and Service/Test remain distinct controls.
- Input snapshots are consumed safely by the emulation thread, with callbacks made from the appropriate execution context.
- Frame stepping, geometry, audio quantity, and buffering remain consistent through normal execution and frontend pause/resume.
- Reset, close/reopen, and deinit/init complete without stale input, stale game state, leaked workers, or hangs.
- SRAM and other supported writable state persist across a normal close/reopen without overwriting original reference data.

**Deferred:** speed targets, additional renderers, full peripheral coverage, savestates, rewind, runahead, and netplay. Savestate-based features require their own state-completeness and memory-budget work before being advertised.

## M4 — Investigate scheduling

**Purpose:** measure the working core before changing its threading model.

Compare the PCEE2-derived integration with paraLLEl-PS2's CPU/GS coordination, GS queue ownership, queue draining, frame gates, MTVU interactions, backpressure, and graphics-context lifetime. Similar function names do not establish interchangeable behavior.

Use a short, repeatable workload at the same content checkpoint and settings. Startup and loading are measured separately from active gameplay. Capture frame time and variation, blocking at synchronization points, queue behavior, and input/frame association as relevant to the proposed change.

**Exit criteria:**

- A written comparison explains producer/consumer ownership, presentation ownership, and progress during pause, stop, context loss, and startup failure.
- Each proposed change names a measured problem, changes one relevant scheduling choice at a time, and includes before/after evidence.
- Accepted changes preserve M3 behavior and lifecycle guarantees; unsupported or regressing experiments are rejected.
- Keeping the initial scheduling model is an acceptable result when measurements do not justify a replacement.

**Deferred:** an automatic wholesale switch to paraLLEl-PS2's model or a general speed claim based only on boot progress or CPU utilization.

## M5 — Performance and compatibility

**Purpose:** expand the working core through independently reviewable improvements.

Candidate work includes CPU/GS/MTVU efficiency, hardware presentation and graphics backends, System256/Super256 coverage, additional media and games, ARM64, VFS coverage, lightguns, wheels, drums, touch input, and other arcade devices. A successful check of one game does not establish full board coverage. These are candidate workstreams, not a promise that all are required for one release.

Each selected workstream receives its own later TODO and acceptance criteria. Performance work identifies the workload and comparison settings; platform work identifies the architecture and frontend; compatibility work identifies the game/device case and standalone comparison.

**Acceptance principle:** improvements show the claimed benefit on their target case while preserving the relevant functional baseline. Compatibility fixes and performance improvements are evaluated separately when their evidence differs.

## Baseline maintenance and future TODOs

Reference revisions remain fixed during a milestone unless an explicit integration decision changes them. A reference update records the old/new revisions, the reason, affected behavior, and focused checks to repeat. Upstream synchronization should remain distinguishable from libretro-specific changes.

Before implementing a milestone, its separate TODO will identify:

- Entry conditions, the exact problem, and exclusions.
- Source revisions, affected interfaces, and upstream changes being reused.
- Small implementation steps and expected artifacts.
- A bounded validation case, acceptance evidence, and conditions for stopping an unsuccessful approach.

Routine validation stops once the agreed checkpoint, input behavior, and normal shutdown have been established. Longer gameplay runs or broader repetition need a specific unresolved issue or performance question. Milestone checklists track reviewable project artifacts and implementation scope. Machine-specific execution results and acceptance records remain outside this repository.
