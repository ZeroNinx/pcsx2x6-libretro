// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroSession.h"
#include "LibretroHost.h"

#include "MTGS.h"
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

	enum class SessionState
	{
		None,
		Starting,
		Active,
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

	std::optional<VMBootParameters> s_boot_request;
	bool s_boot_done = false;
	bool s_boot_succeeded = false;
	std::string s_boot_error;

	SessionState s_session_state = SessionState::None;
	bool s_stop_requested = false;
	bool s_run_token = false;
	u64 s_frame_count = 0;

	// Frames rotate through three buffers: the CPU thread captures into s_capture_frame,
	// the session lock guards s_published_frame, and the frontend presents its own buffer.
	// Exchanging them keeps the CPU thread from writing pixels that are being presented.
	std::vector<u32> s_capture_frame;
	std::vector<u32> s_published_frame;
	bool s_frame_published = false;
	u32 s_capture_failures = 0;

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

// While paused, the VM does not reach a vsync, so requests are served here.
static void WaitWhilePaused()
{
	std::unique_lock lock(s_mutex);
	s_cv.wait(lock, []() { return s_stop_requested || !s_cpu_work.empty(); });
	if (!s_cpu_work.empty())
	{
		RunQueuedWork(lock);
		return;
	}

	lock.unlock();
	VMManager::SetState(VMState::Stopping);
}

// No frame of a session may reach the frontend after that session. Requires s_mutex.
static void ClearPublishedFrame()
{
	s_published_frame.clear();
	s_frame_published = false;
	s_capture_failures = 0;
}

// Reads back the frame whose vsync the CPU thread has just queued, so the GS has merged it
// by the time it runs the readback. Runs without the session lock while the GS thread works.
static bool CaptureFrame()
{
	using LibretroSession::FRAME_HEIGHT;
	using LibretroSession::FRAME_WIDTH;

	u32 width, height;
	if (!MTGS::SaveMemorySnapshot(FRAME_WIDTH, FRAME_HEIGHT, true, false, &width, &height, &s_capture_frame))
		return false;

	if (width != FRAME_WIDTH || height != FRAME_HEIGHT)
	{
		Console.ErrorFmt("Frame readback returned {}x{} instead of {}x{}", width, height, FRAME_WIDTH, FRAME_HEIGHT);
		return false;
	}

	// The readback holds R, G, B, A bytes; XRGB8888 is B, G, R, X in little-endian memory.
	for (u32& pixel : s_capture_frame)
		pixel = (pixel & 0xFF00FF00u) | ((pixel >> 16) & 0xFFu) | ((pixel & 0xFFu) << 16);

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
		s_frame_count = 0;
		s_run_token = false;
		ClearPublishedFrame();
	}
	FinishBoot(true, std::string());
	Console.WriteLn("VM started");

	// Initialize() leaves a successfully started VM paused; anything else breaks the
	// state handling below.
	if (VMManager::GetState() != VMState::Paused)
		Console.ErrorFmt("VM state after initialization is {}, expected Paused", static_cast<int>(VMManager::GetState()));

	VMManager::SetState(VMState::Running);
	for (bool running = true; running;)
	{
		switch (VMManager::GetState())
		{
			case VMState::Running:
				VMManager::Execute();
				break;

			case VMState::Resetting:
				VMManager::Reset();
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
	Console.WriteLn("VM shut down");

	std::unique_lock lock(s_mutex);
	s_session_state = SessionState::None;
	s_stop_requested = false;
	s_run_token = false;
	ClearPublishedFrame();
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

bool LibretroSession::Start(VMBootParameters boot_params, Error* error)
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

	// From here on, a stop request cancels the startup.
	s_session_state = SessionState::Starting;
	s_boot_request = std::move(boot_params);
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

void LibretroSession::RunFrame()
{
	std::unique_lock lock(s_mutex);
	if (s_session_state != SessionState::Active)
		return;

	const u64 next_frame = s_frame_count + 1;
	s_run_token = true;
	s_cv.notify_all();

	WaitWithDiagnostics(lock, "the next frame",
		[next_frame]() { return s_frame_count >= next_frame || s_session_state != SessionState::Active; });
}

bool LibretroSession::TakeFrame(std::vector<u32>* pixels)
{
	std::unique_lock lock(s_mutex);
	if (!s_frame_published)
		return false;

	s_published_frame.swap(*pixels);
	s_frame_published = false;
	return true;
}

void LibretroSession::Stop()
{
	std::unique_lock lock(s_mutex);
	if (s_session_state == SessionState::None)
		return;

	s_stop_requested = true;
	s_cv.notify_all();

	WaitWithDiagnostics(lock, "VM shutdown", []() { return s_session_state == SessionState::None; });
}

void LibretroSession::Shutdown()
{
	Stop();

	std::unique_lock lock(s_mutex);
	if (s_worker_state == WorkerState::None)
		return;

	s_exit_requested = true;
	s_cv.notify_all();

	WaitWithDiagnostics(lock, "CPU thread shutdown", []() { return s_worker_state == WorkerState::Exited; });
	JoinCPUThread(lock);
	s_exit_requested = false;

	// The CPU thread has ended, so nothing captures into these buffers any more.
	std::vector<u32>().swap(s_capture_frame);
	std::vector<u32>().swap(s_published_frame);
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
	const bool captured = CaptureFrame();

	std::unique_lock lock(s_mutex);
	if (captured)
	{
		if (s_capture_failures >= CAPTURE_FAILURE_REPORT_FRAMES)
			Console.WriteLnFmt("Frame readback recovered after {} frames without an image", s_capture_failures);

		s_capture_failures = 0;
		s_published_frame.swap(s_capture_frame);
		s_frame_published = true;
	}
	else if (++s_capture_failures % CAPTURE_FAILURE_REPORT_FRAMES == 0)
	{
		// Frames without an image are expected while the GS has nothing to show yet; a long
		// run of them means the frontend keeps presenting an old or black frame.
		Console.WarningFmt("No frame could be read back for {} frames", s_capture_failures);
	}

	// The frame is published before the frontend learns that the vsync completed.
	s_frame_count++;
	s_cv.notify_all();

	for (;;)
	{
		if (!s_cpu_work.empty())
		{
			RunQueuedWork(lock);
			continue;
		}

		if (s_stop_requested)
		{
			lock.unlock();
			VMManager::SetState(VMState::Stopping);
			return;
		}

		// Queued work may have paused or reset the VM; execution has to leave for that.
		if (VMManager::GetState() != VMState::Running)
			return;

		if (s_run_token)
		{
			s_run_token = false;
			return;
		}

		s_cv.wait(lock);
	}
}
