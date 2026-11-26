// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroSession.h"
#include "LibretroHost.h"
#include "LibretroInput.h"
#include "LibretroOutput.h"

#include "GS.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "MTGS.h"
#include "MTVU.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/Threading.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace
{
	using Clock = std::chrono::steady_clock;

	// Waits outside the CPU thread wake this often, forwarding logs on the frontend
	// thread, and report a stall at the watchdog interval. A stall is diagnosed, never abandoned: the CPU thread runs
	// code in this library and must finish before the library can be unloaded.
	constexpr auto WAIT_INTERVAL = std::chrono::milliseconds(100);
	constexpr auto WATCHDOG_INTERVAL = std::chrono::seconds(10);
	// About ten seconds of emulated frames.
	constexpr u32 CAPTURE_FAILURE_REPORT_FRAMES = 600;

	enum class FrameRequestWait
	{
		Advance,
		Stop,
		StateChanged,
		StateRequest,
	};

	// A state operation the frontend thread waits for. The work and error belong to the
	// waiting frontend thread, which keeps them alive until the result is set.
	struct StateRequest
	{
		bool loads = false;
		const std::function<LibretroSession::StateResult(Error*)>* work = nullptr;
		Error* error = nullptr;
		std::optional<LibretroSession::StateResult> result;
	};

	enum class SessionState
	{
		None,
		Starting,
		Active,
	};

	// Progress of a frontend reset request.
	enum class ResetRequest
	{
		None,
		Pending,
		Completed,
		Refused,
	};

	enum class WorkerState
	{
		None,
		Starting,
		Ready,
		Failed,
		Exited,
	};

	std::mutex s_mutex;
	// Every state change notifies all waiters, so a stop request reaches whichever
	// wait the CPU thread is in.
	std::condition_variable s_cv;

	Threading::Thread s_cpu_thread;
	std::thread::id s_cpu_thread_id;
	WorkerState s_worker_state = WorkerState::None;
	bool s_exit_requested = false;

	std::deque<std::function<void()>> s_cpu_work;

	// Boot parameters of the loaded content until Start() boots them.
	std::optional<VMBootParameters> s_prepared_boot;
	// Largest frame the loaded content may publish. Set by Prepare() before the session starts,
	// read by the CPU thread while it runs, and returned to native resolution by Stop().
	LibretroSession::FrameSize s_max_frame_size = LibretroSession::GetMaxFrameSize(1);
	std::optional<VMBootParameters> s_boot_request;
	bool s_boot_done = false;
	bool s_boot_succeeded = false;
	std::string s_boot_error;

	SessionState s_session_state = SessionState::None;
	bool s_stop_requested = false;

	// Frame requests and completions, counted per session. The CPU thread advances only
	// while a request is pending, so every request yields exactly one vsync.
	u64 s_requested_frames = 0;
	u64 s_completed_frames = 0;
	// Input and output slot of the latest request; the CPU thread copies them into s_frame_input
	// and s_frame_slot when it starts that frame, and only the CPU thread reads those.
	LibretroSession::InputSnapshot s_requested_input;
	LibretroSession::InputSnapshot s_frame_input;
	u32 s_requested_slot = 0;
	u32 s_frame_slot = 0;
	// Device changes of requests a paused VM completed without applying their input; they
	// reach the input of the next frame the VM runs, so no reconnect goes unseen.
	std::array<bool, LibretroSession::INPUT_PORTS> s_unapplied_device_changes = {};
	// Whether the latest completed request advanced the VM to a vsync.
	bool s_request_advanced = false;
	// The pending state operation, if any.
	std::optional<StateRequest> s_state_request;
	// Changes with every change of the VM state; see GetStateGeneration().
	u64 s_state_generation = 0;
	// CPU thread only: whether VMManager::Execute() is on the stack, so that a state request
	// first has to leave it.
	bool s_executing = false;
	// CPU thread only: whether the VM may run to its next vsync. Only a frame request grants
	// this, so starting, resetting or resuming the VM never runs a frame nobody requested.
	bool s_frame_granted = false;
	ResetRequest s_reset_request = ResetRequest::None;

	// The latest completed frame, guarded by the session lock until the frontend takes it.
	LibretroSession::Frame s_published_frame;
	bool s_frame_published = false;
	u32 s_capture_failures = 0;
	// Published at every completion, so timing is known before the first image exists.
	double s_vsync_frequency = 0.0;
	// Audio of the completions the frontend has not taken yet.
	LibretroAudio::Samples s_published_audio;

	// The thread that calls the libretro entry points; only it may forward logs.
	std::thread::id s_frontend_thread_id;
} // namespace

// Every wait outside the CPU thread goes through here. A wait on the frontend thread
// also forwards logs; any wait reports a stall but is never abandoned.
template <typename Predicate>
static void WaitWithDiagnostics(std::unique_lock<std::mutex>& lock, const char* description, Predicate predicate)
{
	const bool on_frontend_thread = (std::this_thread::get_id() == s_frontend_thread_id);
	const Clock::time_point start = Clock::now();
	Clock::time_point next_report = start + WATCHDOG_INTERVAL;
	while (!s_cv.wait_for(lock, WAIT_INTERVAL, predicate))
	{
		lock.unlock();
		if (on_frontend_thread)
			LibretroHost::DrainLogMessages();

		const Clock::time_point now = Clock::now();
		if (now >= next_report)
		{
			const auto waited = std::chrono::duration_cast<std::chrono::seconds>(now - start);
			Console.WarningFmt("Still waiting for {} after {} seconds", description, waited.count());
			next_report += WATCHDOG_INTERVAL;
		}

		lock.lock();
	}

	if (on_frontend_thread)
	{
		lock.unlock();
		LibretroHost::DrainLogMessages();
		lock.lock();
	}
}

static bool IsOnCPUThread()
{
	std::unique_lock lock(s_mutex);
	return (s_cpu_thread_id == std::this_thread::get_id());
}

static void RunQueuedWork(std::unique_lock<std::mutex>& lock)
{
	std::function<void()> work = std::move(s_cpu_work.front());
	s_cpu_work.pop_front();

	lock.unlock();
	work();
	lock.lock();
}

static void FinishBoot(bool succeeded, std::string error)
{
	std::unique_lock lock(s_mutex);
	s_boot_done = true;
	s_boot_succeeded = succeeded;
	s_boot_error = std::move(error);
	s_session_state = succeeded ? SessionState::Active : SessionState::None;
	if (!succeeded)
		s_stop_requested = false;
	s_cv.notify_all();
}

static bool HasPendingStateRequest()
{
	return s_state_request.has_value() && !s_state_request->result.has_value();
}

// CPU thread, outside VM execution: carries out the pending state request. The VM stays in its
// running or paused state; a running VM waits for a new frame grant before it runs.
static void HandleStateRequest(std::unique_lock<std::mutex>& lock)
{
	const StateRequest request = *s_state_request;
	lock.unlock();

	if (THREAD_VU1)
		vu1Thread.WaitVU();
	MTGS::WaitGS(false);

	const LibretroSession::StateResult result = (*request.work)(request.error);
	if (request.loads && result == LibretroSession::StateResult::Done)
	{
		// The replaced state's held input and armed edges, and its output not yet taken by
		// the frontend, do not carry over.
		LibretroInput::Reset();
		std::unique_lock output_lock(s_mutex);
		s_state_generation++;
		LibretroAudio::DrainCommitted(&s_published_audio);
		s_published_audio.discarded_frames += s_published_audio.GetFrameCount();
		s_published_audio.data.clear();
		s_frame_published = false;
		s_capture_failures = 0;
	}
	s_frame_granted = false;

	lock.lock();
	s_state_request->result = result;
	s_cv.notify_all();
}

// While paused, the VM does not reach a vsync, so work and stop requests are served here.
// Frame requests complete without advancing the VM: the frontend presents the last frame
// again and receives no audio.
static void WaitWhilePaused()
{
	std::unique_lock lock(s_mutex);
	s_cv.wait(lock, []() {
		return s_stop_requested || !s_cpu_work.empty() || HasPendingStateRequest() || s_requested_frames > s_completed_frames;
	});
	if (!s_cpu_work.empty())
	{
		RunQueuedWork(lock);
		return;
	}

	if (HasPendingStateRequest())
	{
		HandleStateRequest(lock);
		return;
	}

	if (s_stop_requested)
	{
		lock.unlock();
		VMManager::SetState(VMState::Stopping);
		return;
	}

	for (u32 port = 0; port < LibretroSession::INPUT_PORTS; port++)
		s_unapplied_device_changes[port] |= s_requested_input.device_changed[port];
	s_completed_frames = s_requested_frames;
	s_request_advanced = false;
	s_cv.notify_all();
}

// No frame, request or input of a session may outlive it. Requires s_mutex.
static void ResetFrameState()
{
	s_state_generation++;
	s_requested_frames = 0;
	s_completed_frames = 0;
	s_requested_input = {};
	s_frame_input = {};
	s_requested_slot = 0;
	s_frame_slot = 0;
	s_unapplied_device_changes = {};
	s_request_advanced = false;
	s_published_frame = {};
	s_frame_published = false;
	s_capture_failures = 0;
	s_vsync_frequency = 0.0;
	s_published_audio.data.clear();
	s_published_audio.sample_rate = 0;
	s_published_audio.discarded_frames = 0;
	s_frame_granted = false;
	s_reset_request = ResetRequest::None;
}

// Parks the running VM at a frame boundary until the frontend requests a frame, serving
// queued work meanwhile. When queued work changes the VM state, the caller's state handling
// takes over. Requires s_mutex.
static FrameRequestWait WaitForFrameRequest(std::unique_lock<std::mutex>& lock)
{
	for (;;)
	{
		if (!s_cpu_work.empty())
		{
			RunQueuedWork(lock);
			continue;
		}

		if (s_stop_requested)
			return FrameRequestWait::Stop;

		if (VMManager::GetState() != VMState::Running)
			return FrameRequestWait::StateChanged;

		if (HasPendingStateRequest())
		{
			// Inside execution the request is only taken once the VM has left it.
			if (s_executing)
				return FrameRequestWait::StateRequest;

			HandleStateRequest(lock);
			continue;
		}

		if (s_requested_frames > s_completed_frames)
		{
			s_frame_input = s_requested_input;
			s_frame_slot = s_requested_slot;
			for (u32 port = 0; port < LibretroSession::INPUT_PORTS; port++)
				s_frame_input.device_changed[port] |= std::exchange(s_unapplied_device_changes[port], false);
			return FrameRequestWait::Advance;
		}

		s_cv.wait(lock);
	}
}

// CPU thread: waits for the next frame request. Returns true when the VM may run to its next
// vsync, with the request's input applied; a stop request stops the VM instead.
static bool AwaitFrameGrant()
{
	FrameRequestWait wait;
	{
		std::unique_lock lock(s_mutex);
		wait = WaitForFrameRequest(lock);
	}

	if (wait == FrameRequestWait::Advance)
	{
		LibretroInput::Apply(s_frame_input);
		return true;
	}

	if (wait == FrameRequestWait::Stop)
		VMManager::SetState(VMState::Stopping);
	else if (wait == FrameRequestWait::StateRequest)
	{
		// The current event finishes, so the vsync completes as usual, before execution
		// returns to the session loop, where the request is served without a frame grant.
		Cpu->ExitExecution();
	}
	return false;
}

// CPU thread, after the VM has actually reset: nothing produced before the reset reaches the
// frontend afterwards, no control stays held, and the VM waits for a new frame request.
static void FinishReset()
{
	LibretroInput::Reset();
	s_frame_granted = false;

	std::unique_lock lock(s_mutex);
	s_state_generation++;
	// Audio SPU2 committed after the last completed frame belongs to the run that ended.
	LibretroAudio::DrainCommitted(&s_published_audio);
	s_published_audio.discarded_frames += s_published_audio.GetFrameCount();
	s_published_audio.data.clear();
	s_frame_published = false;
	s_capture_failures = 0;
	if (s_reset_request == ResetRequest::Pending)
		s_reset_request = ResetRequest::Completed;
	Console.WriteLnFmt("VM reset after {} frames", s_completed_frames);
	s_cv.notify_all();
}

// CPU thread: carries out a frontend reset request. A paused VM resets right away and stays
// paused; a running VM first leaves execution, and the session loop completes the reset.
static void RequestVMReset()
{
	const VMState state = VMManager::GetState();
	bool accepted = false;
	if (state == VMState::Running || state == VMState::Paused)
		accepted = VMManager::RequestReset();
	else
		Console.ErrorFmt("A reset was requested while the VM is in state {}", static_cast<int>(state));

	if (!accepted)
	{
		Console.Warning("The VM refused the reset; the session continues unchanged");
		std::unique_lock lock(s_mutex);
		if (s_reset_request == ResetRequest::Pending)
			s_reset_request = ResetRequest::Refused;
		s_cv.notify_all();
		return;
	}

	if (state == VMState::Paused)
		FinishReset();
}

// Draws the frame whose vsync the CPU thread has just queued into its output slot, so the GS
// has merged it by the time it runs the job. Returns whether a new image was drawn. Runs
// without the session lock while the GS thread works; waits for the GS to submit the drawing,
// not for the GPU to finish it.
static bool CaptureFrame(LibretroSession::Frame* frame)
{
	using namespace LibretroSession;

	// The aspect ratio is read in the same GS job as the image, so both describe it.
	const u32 slot = s_frame_slot;
	u32 width = 0, height = 0;
	float aspect_ratio = 0.0f;
	LibretroOutput::Image image;
	LibretroOutput::Result result = LibretroOutput::Result::Unchanged;
	MTGS::RunOnGSThread([slot, &image, &aspect_ratio, &result]() {
		result = LibretroOutput::RenderOutput(slot, &image);
		aspect_ratio = GSGetDisplayAspectRatio();
	});
	MTGS::WaitGS(false, false, false);
	if (result != LibretroOutput::Result::Drawn)
		return false;
	width = image.width;
	height = image.height;

	// The frontend relies on these sizes, so a frame outside them is never published.
	if (width == 0 || height == 0 || width > s_max_frame_size.width || height > s_max_frame_size.height ||
		!(aspect_ratio > 0.0f))
	{
		Console.ErrorFmt("The GS drew a {}x{} frame at aspect ratio {}; the frame is dropped", width, height, aspect_ratio);
		return false;
	}

	frame->slot = image.slot;
	frame->generation = image.generation;
	frame->width = width;
	frame->height = height;
	frame->aspect_ratio = aspect_ratio;
	return true;
}

static void RunSession(const VMBootParameters& boot_params)
{
	VMManager::ApplySettings();

	Error error;
	const VMBootResult result = VMManager::Initialize(boot_params, &error);
	if (result != VMBootResult::StartupSuccess)
	{
		FinishBoot(false, error.IsValid() ? error.GetDescription() : std::string("VM initialization failed."));
		return;
	}

	// A stop requested during initialization, which cannot interrupt it, wins over its success.
	bool cancelled;
	{
		std::unique_lock lock(s_mutex);
		cancelled = s_stop_requested;
	}
	if (cancelled)
	{
		Console.Warning("VM startup was cancelled");
		VMManager::Shutdown(false);
		FinishBoot(false, "VM startup was cancelled.");
		return;
	}

	{
		std::unique_lock lock(s_mutex);
		ResetFrameState();
	}
	FinishBoot(true, std::string());
	Console.WriteLn("VM started");

	// Initialize() leaves a successfully started VM paused; anything else breaks the
	// state handling below.
	if (VMManager::GetState() != VMState::Paused)
		Console.ErrorFmt("VM state after initialization is {}, expected Paused", static_cast<int>(VMManager::GetState()));

	// The layout belongs to the content this session parsed; no control is held at its start.
	LibretroInput::Reset();

	// The session runs the VM, which still executes nothing before the first frame request.
	VMManager::SetState(VMState::Running);

	for (bool running = true; running;)
	{
		switch (VMManager::GetState())
		{
			case VMState::Running:
				if (!s_frame_granted)
					s_frame_granted = AwaitFrameGrant();
				if (s_frame_granted)
				{
					s_executing = true;
					VMManager::Execute();
					s_executing = false;
				}
				break;

			case VMState::Resetting:
				VMManager::Reset();
				FinishReset();
				break;

			case VMState::Paused:
				WaitWhilePaused();
				break;

			case VMState::Stopping:
				running = false;
				break;

			default:
				Console.ErrorFmt("Unexpected VM state {} in an active session", static_cast<int>(VMManager::GetState()));
				running = false;
				break;
		}
	}

	VMManager::Shutdown(false);
	std::unique_lock lock(s_mutex);
	Console.WriteLnFmt("VM shut down after {} frames", s_completed_frames);
	s_session_state = SessionState::None;
	s_stop_requested = false;
	ResetFrameState();
	s_cv.notify_all();
}

static void CPUThreadLoop()
{
	std::unique_lock lock(s_mutex);
	for (;;)
	{
		s_cv.wait(lock, []() { return s_exit_requested || s_boot_request.has_value() || !s_cpu_work.empty(); });

		if (!s_cpu_work.empty())
		{
			RunQueuedWork(lock);
		}
		else if (s_boot_request.has_value())
		{
			const VMBootParameters boot_params = std::move(*s_boot_request);
			s_boot_request.reset();

			lock.unlock();
			RunSession(boot_params);
			lock.lock();
		}
		else
		{
			break;
		}
	}
}

static void CPUThreadEntry()
{
	{
		std::unique_lock lock(s_mutex);
		s_cpu_thread_id = std::this_thread::get_id();
	}

	const bool initialized = VMManager::Internal::CPUThreadInitialize();
	{
		std::unique_lock lock(s_mutex);
		s_worker_state = initialized ? WorkerState::Ready : WorkerState::Failed;
		s_cv.notify_all();
	}

	if (initialized)
	{
		CPUThreadLoop();
		VMManager::Internal::CPUThreadShutdown();
	}

	std::unique_lock lock(s_mutex);
	if (initialized)
		s_worker_state = WorkerState::Exited;
	s_cpu_thread_id = std::thread::id();
	s_cv.notify_all();
}

static void JoinCPUThread(std::unique_lock<std::mutex>& lock)
{
	lock.unlock();
	s_cpu_thread.Join();
	lock.lock();
	s_worker_state = WorkerState::None;
}

static bool EnsureCPUThread(std::unique_lock<std::mutex>& lock, Error* error)
{
	if (s_worker_state == WorkerState::Ready)
		return true;

	s_worker_state = WorkerState::Starting;
	s_exit_requested = false;

	lock.unlock();
	s_cpu_thread.SetStackSize(VMManager::EMU_THREAD_STACK_SIZE);
	const bool started = s_cpu_thread.Start(&CPUThreadEntry);
	lock.lock();

	if (!started)
	{
		s_worker_state = WorkerState::None;
		Error::SetString(error, "Failed to create the CPU thread.");
		return false;
	}

	WaitWithDiagnostics(lock, "CPU thread initialization", []() { return s_worker_state != WorkerState::Starting; });
	if (s_worker_state == WorkerState::Failed)
	{
		// Nothing is held after a failed initialization; the next load starts a new thread.
		JoinCPUThread(lock);
		Error::SetString(error, "CPU thread initialization failed.");
		return false;
	}

	return true;
}

bool LibretroSession::Prepare(VMBootParameters boot_params, u32 upscale_multiplier, Error* error)
{
	std::unique_lock lock(s_mutex);
	s_frontend_thread_id = std::this_thread::get_id();
	if (s_session_state != SessionState::None)
	{
		Error::SetString(error, "A VM session is already active.");
		return false;
	}

	if (!EnsureCPUThread(lock, error))
		return false;

	s_prepared_boot = std::move(boot_params);
	s_max_frame_size = GetMaxFrameSize(upscale_multiplier);
	return true;
}

// CPU thread, once the GS opened: the GS clamps the configured multiplier to what it derives
// from the device's texture size limit. That limit does not prove every allocation fits, so
// frames keep their own validation and allocation failures their own reports.
static void LogUpscaleMultiplier()
{
	float effective = 0.0f;
	u32 max_texture_size = 0;
	MTGS::RunOnGSThread([&effective, &max_texture_size]() {
		effective = GSConfig.UpscaleMultiplier;
		max_texture_size = g_gs_device->GetMaxTextureSize();
	});
	MTGS::WaitGS(false, false, false);

	const float selected = EmuConfig.GS.UpscaleMultiplier;
	if (effective != selected)
		Console.WarningFmt("Internal resolution {}x selected, but the GS renders at {}x; maximum texture size {}",
			selected, effective, max_texture_size);
	else
		Console.WriteLnFmt("Internal resolution {}x; maximum texture size {}", effective, max_texture_size);
}

bool LibretroSession::OpenGS(Error* error)
{
	bool opened = false;
	RunOnCPUThread(
		[&opened]() {
			// The GS reads its configuration when it opens.
			VMManager::ApplySettings();
			opened = MTGS::WaitForOpen();
			if (opened)
				LogUpscaleMultiplier();
		},
		true);

	if (!opened)
		Error::SetString(error, "The GS failed to open on the shared graphics device.");
	return opened;
}

bool LibretroSession::Start(Error* error)
{
	std::unique_lock lock(s_mutex);
	if (s_session_state != SessionState::None)
	{
		Error::SetString(error, "A VM session is already active.");
		return false;
	}

	if (!s_prepared_boot.has_value())
	{
		Console.Error("A VM start was requested without prepared content");
		Error::SetString(error, "No content is prepared to start.");
		return false;
	}

	// From here on, a stop request cancels the startup.
	s_session_state = SessionState::Starting;
	s_boot_request = std::move(s_prepared_boot);
	s_prepared_boot.reset();
	s_boot_done = false;
	s_stop_requested = false;
	s_cv.notify_all();

	WaitWithDiagnostics(lock, "VM startup", []() { return s_boot_done; });
	if (!s_boot_succeeded)
	{
		Error::SetString(error, s_boot_error);
		return false;
	}

	return true;
}

void LibretroSession::CloseGS()
{
	{
		std::unique_lock lock(s_mutex);
		// Only the CPU thread opens the GS, so without one there is nothing to close.
		if (s_worker_state != WorkerState::Ready)
			return;
		if (s_session_state != SessionState::None)
		{
			Console.Error("The GS cannot close while the VM session is active");
			return;
		}
	}

	RunOnCPUThread(
		[]() {
			if (!MTGS::IsOpen())
				return;

			// The output images live on the GS device, so they go before it.
			MTGS::RunOnGSThread(&LibretroOutput::ReleaseOutputs);
			MTGS::WaitGS(false, false, false);
			MTGS::WaitForClose();
		},
		true);
}

LibretroSession::FrameResult LibretroSession::RunFrame(
	const InputSnapshot& input, u32 output_slot, LibretroAudio::Samples* audio)
{
	FrameResult result;
	std::unique_lock lock(s_mutex);
	if (s_session_state == SessionState::Active)
	{
		s_requested_input = input;
		s_requested_slot = output_slot;
		const u64 request = ++s_requested_frames;
		s_cv.notify_all();

		WaitWithDiagnostics(lock, "the next frame",
			[request]() { return s_completed_frames >= request || s_session_state != SessionState::Active; });
		result.advanced = (s_completed_frames >= request && s_request_advanced);
	}

	// Everything published up to now is taken under the same lock, so it describes one point.
	if (s_frame_published)
	{
		result.new_frame = true;
		result.frame = s_published_frame;
		s_frame_published = false;
	}
	result.vsync_frequency = s_vsync_frequency;
	if (result.advanced)
	{
		audio->data.clear();
		audio->discarded_frames = 0;
		std::swap(s_published_audio, *audio);
	}
	return result;
}

bool LibretroSession::Reset()
{
	std::unique_lock lock(s_mutex);
	if (s_session_state != SessionState::Active)
	{
		Console.Warning("A reset was requested without an active VM session");
		return false;
	}

	s_reset_request = ResetRequest::Pending;
	s_cpu_work.push_back(&RequestVMReset);
	s_cv.notify_all();

	WaitWithDiagnostics(lock, "the VM reset",
		[]() { return s_reset_request != ResetRequest::Pending || s_session_state != SessionState::Active; });
	const bool completed = (s_reset_request == ResetRequest::Completed);
	s_reset_request = ResetRequest::None;
	return completed;
}

LibretroSession::StateResult LibretroSession::RunStateOperation(
	bool loads, const std::function<StateResult(Error*)>& work, Error* error)
{
	std::unique_lock lock(s_mutex);
	if (s_session_state != SessionState::Active)
	{
		Error::SetString(error, "No content is running.");
		return StateResult::Rejected;
	}

	s_state_request.emplace();
	s_state_request->loads = loads;
	s_state_request->work = &work;
	s_state_request->error = error;
	s_cv.notify_all();

	WaitWithDiagnostics(lock, loads ? "the state load" : "the state save",
		[]() { return s_state_request->result.has_value() || s_session_state != SessionState::Active; });

	const std::optional<StateResult> result = s_state_request->result;
	s_state_request.reset();
	if (!result.has_value())
	{
		Error::SetString(error, "The VM session ended before the state operation ran.");
		return StateResult::Rejected;
	}
	return *result;
}

u64 LibretroSession::GetStateGeneration()
{
	std::unique_lock lock(s_mutex);
	return s_state_generation;
}

bool LibretroSession::DrawBlackFrame(u32 output_slot, Frame* frame)
{
	if (!IsActive())
		return false;

	// The GS belongs to the CPU thread, which serves this work whether the VM is parked or paused.
	LibretroOutput::Result result = LibretroOutput::Result::Failed;
	LibretroOutput::Image image;
	RunOnCPUThread(
		[output_slot, &result, &image]() {
			MTGS::RunOnGSThread([output_slot, &result, &image]() {
				result = LibretroOutput::RenderBlackOutput(output_slot, &image);
			});
			MTGS::WaitGS(false, false, false);
		},
		true);
	if (result != LibretroOutput::Result::Drawn)
		return false;

	const u32 width = image.width;
	const u32 height = image.height;
	frame->slot = image.slot;
	frame->generation = image.generation;
	frame->width = width;
	frame->height = height;
	frame->aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
	return true;
}

void LibretroSession::Stop()
{
	std::unique_lock lock(s_mutex);
	s_prepared_boot.reset();
	if (s_session_state != SessionState::None)
	{
		s_stop_requested = true;
		s_cv.notify_all();

		WaitWithDiagnostics(lock, "VM shutdown", []() { return s_session_state == SessionState::None; });
	}
	s_max_frame_size = GetMaxFrameSize(1);
}

void LibretroSession::Shutdown()
{
	Stop();
	CloseGS();

	std::unique_lock lock(s_mutex);
	if (s_worker_state == WorkerState::None)
		return;

	s_exit_requested = true;
	s_cv.notify_all();

	WaitWithDiagnostics(lock, "CPU thread shutdown", []() { return s_worker_state == WorkerState::Exited; });
	JoinCPUThread(lock);
	s_exit_requested = false;
}

void LibretroSession::ForgetTerminatedThreads()
{
	if (s_cpu_thread.Joinable())
		s_cpu_thread.Detach();
	MTGS::ForgetTerminatedThread();
}

bool LibretroSession::IsActive()
{
	std::unique_lock lock(s_mutex);
	return (s_session_state == SessionState::Active);
}

void LibretroSession::RunOnCPUThread(std::function<void()> function, bool block)
{
	if (block && IsOnCPUThread())
	{
		function();
		return;
	}

	std::unique_lock lock(s_mutex);
	if (s_worker_state != WorkerState::Ready)
	{
		Console.Error("Dropped a CPU thread request: no CPU thread is running");
		return;
	}

	if (!block)
	{
		s_cpu_work.push_back(std::move(function));
		s_cv.notify_all();
		return;
	}

	bool done = false;
	s_cpu_work.push_back([&function, &done]() {
		function();
		std::unique_lock lock(s_mutex);
		done = true;
		s_cv.notify_all();
	});
	s_cv.notify_all();
	WaitWithDiagnostics(lock, "CPU thread work", [&done]() { return done; });
}

void LibretroSession::RequestStop()
{
	std::unique_lock lock(s_mutex);
	if (s_session_state == SessionState::None)
		return;

	s_stop_requested = true;
	s_cv.notify_all();
}

void LibretroSession::PumpCPUThread()
{
	LibretroSession::Frame frame;
	const bool captured = CaptureFrame(&frame);
	// Until the BIOS sets a video mode, the emulator only has a placeholder frequency.
	const double vsync_frequency = (gsVideoMode != GS_VideoMode::Uninitialized) ? GetVerticalFrequency() : 0.0;

	std::unique_lock lock(s_mutex);
	s_vsync_frequency = vsync_frequency;
	// The audio up to this vsync belongs to the frame it completes.
	LibretroAudio::DrainCommitted(&s_published_audio);
	// The VM ran to this vsync, whether or not the frame completes a request.
	s_state_generation++;
	if (s_requested_frames > s_completed_frames)
	{
		s_completed_frames++;
		s_request_advanced = true;
	}
	else
	{
		// The VM only runs on a frame grant, so this means execution resumed without one; the
		// frame is still presented but completes no request.
		Console.Warning("The VM reached a vsync without a frame request");
	}

	if (captured)
	{
		if (s_capture_failures >= CAPTURE_FAILURE_REPORT_FRAMES)
			Console.WriteLnFmt("Frame output recovered after {} frames without a new image", s_capture_failures);

		s_capture_failures = 0;
		frame.sequence = s_completed_frames;
		s_published_frame = frame;
		s_frame_published = true;
	}
	else if (++s_capture_failures % CAPTURE_FAILURE_REPORT_FRAMES == 0)
	{
		// A long run of frames without a new image means the frontend keeps presenting an old one.
		Console.WarningFmt("No new frame image for {} frames", s_capture_failures);
	}

	// The frame is published before the frontend learns that it completed.
	s_cv.notify_all();
	lock.unlock();

	// Queued work may pause or reset the VM; execution then leaves for the state handling,
	// and the VM waits for a new grant before it runs again.
	s_frame_granted = AwaitFrameGrant();
}
