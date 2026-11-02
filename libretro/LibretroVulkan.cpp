// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroVulkan.h"
#include "LibretroSession.h"

#include "GS/Renderers/Vulkan/GSDeviceVK.h"

#include "common/Console.h"
#include "common/Error.h"

#include "libretro_vulkan.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace
{
	retro_environment_t s_environment = nullptr;
	LibretroVulkan::ContextCallbacks s_callbacks = {};

	// The context the GS device runs on. It outlives every device created on it.
	GSDeviceVK::ExternalContext s_context;

	// The frontend's interface while its context is ready. Held from locking the shared queue
	// until unlocking it, so the interface cannot be retired while the GS uses the queue.
	std::mutex s_interface_mutex;
	const retro_hw_render_interface_vulkan* s_interface = nullptr;
	bool s_early_queue_use_reported = false;

	// Set while a frontend callback waits for the GS: the frontend is blocked in that call, so
	// the GS may use the queue without the frontend's lock, which may not exist yet.
	std::atomic<bool> s_frontend_blocked{false};

	// Marks a frontend callback that waits for the GS.
	struct FrontendBlocked
	{
		FrontendBlocked() { s_frontend_blocked.store(true, std::memory_order_release); }
		~FrontendBlocked() { s_frontend_blocked.store(false, std::memory_order_release); }
	};
} // namespace

static void LockQueue()
{
	s_interface_mutex.lock();
	if (s_interface)
		s_interface->lock_queue(s_interface->handle);
	else if (!s_frontend_blocked.load(std::memory_order_acquire) && !std::exchange(s_early_queue_use_reported, true))
		Console.Error("The GS used the shared graphics queue while the frontend's context was not ready");
}

static void UnlockQueue()
{
	if (s_interface)
		s_interface->unlock_queue(s_interface->handle);
	s_interface_mutex.unlock();
}

static void SetInterface(const retro_hw_render_interface_vulkan* iface)
{
	std::unique_lock lock(s_interface_mutex);
	s_interface = iface;
	s_early_queue_use_reported = false;
}

static const VkApplicationInfo* GetApplicationInfo()
{
	static const VkApplicationInfo info = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "PCSX2x6", 0, "PCSX2x6", 0,
		VK_API_VERSION_1_1};
	return &info;
}

// The instance the frontend creates carries the extensions the GS needs, and the GS loads
// every Vulkan function through the frontend's loader.
static VkInstance CreateInstance(PFN_vkGetInstanceProcAddr get_instance_proc_addr, const VkApplicationInfo* app,
	retro_vulkan_create_instance_wrapper_t create_instance_wrapper, void* opaque)
{
	s_context.instance = VK_NULL_HANDLE;
	s_context.physical_device = VK_NULL_HANDLE;
	s_context.get_instance_proc_addr = get_instance_proc_addr;

	// The wrapper and its opaque data are only valid during this call.
	s_context.instance = GSDeviceVK::CreateExternalInstance(get_instance_proc_addr,
		[create_instance_wrapper, opaque](const VkInstanceCreateInfo& create_info) {
			return create_instance_wrapper(opaque, &create_info);
		},
		&s_context.instance_extensions);
	if (s_context.instance == VK_NULL_HANDLE)
		Console.Error("The Vulkan instance for the GS could not be created");
	return s_context.instance;
}

// Negotiation v1 entry point. The frontend only checks that it exists before preferring
// create_device2; the core supports nothing older than v2.
static bool CreateDeviceV1(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char** required_device_extensions,
	unsigned num_required_device_extensions, const char** required_device_layers, unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures* required_features)
{
	Console.Error("The frontend requested a Vulkan device through context negotiation v1, which this core does not "
				  "support; v2 is required");
	return false;
}

// Opens the GS, which creates its device through the frontend's wrapper, and returns that
// device for the frontend to share.
static bool CreateDevice2(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	retro_vulkan_create_device_wrapper_t create_device_wrapper, void* opaque)
{
	if (instance == VK_NULL_HANDLE || instance != s_context.instance)
	{
		Console.Error("The frontend requested a Vulkan device on an instance the core did not create");
		return false;
	}

	const FrontendBlocked frontend_blocked;

	// Every attempt loads the functions again through the resolver of this request: a device
	// that failed to open reset them.
	s_context.get_instance_proc_addr = get_instance_proc_addr;
	s_context.physical_device = gpu;
	s_context.device_handed_over = false;

	// The wrapper and its opaque data are only valid during this call, which waits for the GS.
	s_context.create_device = [create_device_wrapper, opaque](VkPhysicalDevice physical_device,
								  const VkDeviceCreateInfo& create_info) {
		return create_device_wrapper(physical_device, opaque, &create_info);
	};
	GSDeviceVK::SetExternalContext(&s_context);

	Error error;
	const bool opened = LibretroSession::OpenGS(&error);
	s_context.create_device = nullptr;
	if (!opened)
	{
		Console.ErrorFmt("The Vulkan device for the GS could not be created: {}", error.GetDescription());
		GSDeviceVK::SetExternalContext(nullptr);
		return false;
	}

	const GSDeviceVK* const device = GSDeviceVK::GetInstance();
	const VkPhysicalDevice physical_device = device->GetPhysicalDevice();
	const u32 queue_family = device->GetGraphicsQueueFamilyIndex();

	// The frontend presents with the queue it is given, so the GS queue must reach its surface.
	VkBool32 present_supported = VK_TRUE;
	if (surface != VK_NULL_HANDLE &&
		(vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, queue_family, surface, &present_supported) != VK_SUCCESS ||
			!present_supported))
	{
		Console.Error("The GS graphics queue cannot present to the frontend's surface");
		LibretroSession::CloseGS();
		GSDeviceVK::SetExternalContext(nullptr);
		return false;
	}

	context->gpu = physical_device;
	context->device = device->GetDevice();
	context->queue = device->GetGraphicsQueue();
	context->queue_family_index = queue_family;
	context->presentation_queue = context->queue;
	context->presentation_queue_family_index = queue_family;
	s_context.device_handed_over = true;
	Console.WriteLnFmt("The GS shares the frontend's Vulkan device on {}", device->GetName());
	return true;
}

static void ContextReset()
{
	const FrontendBlocked frontend_blocked;
	const retro_hw_render_interface_vulkan* iface = nullptr;
	if (!s_environment(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) || !iface ||
		iface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
		iface->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
	{
		s_callbacks.context_failed("The frontend provided no usable Vulkan render interface.");
		return;
	}

	// Only the device the GS created is shared; a device the frontend made on its own is not.
	const GSDeviceVK* const device = GSDeviceVK::GetInstance();
	if (!device || iface->device != device->GetDevice())
	{
		s_callbacks.context_failed("The frontend's Vulkan context does not use the device the GS created.");
		return;
	}

	SetInterface(iface);
	if (!s_callbacks.context_ready())
		SetInterface(nullptr);
}

// The frontend is about to destroy its context and the device. Nothing may still run on it.
static void ContextDestroy()
{
	s_callbacks.context_lost();
	SetInterface(nullptr);
	GSDeviceVK::SetExternalContext(nullptr);
}

bool LibretroVulkan::Register(retro_environment_t environment, const ContextCallbacks& callbacks)
{
	s_environment = environment;
	s_callbacks = callbacks;
	s_context.lock_queue = &LockQueue;
	s_context.unlock_queue = &UnlockQueue;

	retro_hw_render_context_negotiation_interface support = {RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN, 0};
	if (!environment(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &support) ||
		support.interface_version < 2)
	{
		Console.ErrorFmt("The frontend supports Vulkan context negotiation up to v{}; v2 is required",
			support.interface_version);
		return false;
	}

	static retro_hw_render_callback hw_render = {};
	hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
	hw_render.version_major = 1;
	hw_render.version_minor = 1;
	hw_render.context_reset = &ContextReset;
	hw_render.context_destroy = &ContextDestroy;
	if (!environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
	{
		Console.Error("The frontend cannot provide a Vulkan context");
		return false;
	}

	static retro_hw_render_context_negotiation_interface_vulkan negotiation = {};
	negotiation.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
	negotiation.interface_version = 2;
	negotiation.get_application_info = &GetApplicationInfo;
	negotiation.create_device = &CreateDeviceV1;
	// RetroArch calls destroy_device when it tears down its video driver, which can happen after
	// the core is unloaded; context_destroy already released everything on the device.
	negotiation.destroy_device = nullptr;
	negotiation.create_instance = &CreateInstance;
	negotiation.create_device2 = &CreateDevice2;
	if (!environment(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &negotiation))
	{
		Console.Error("The frontend does not accept the Vulkan context negotiation interface");
		return false;
	}

	return true;
}

const retro_hw_render_interface_vulkan* LibretroVulkan::GetInterface()
{
	std::unique_lock lock(s_interface_mutex);
	return s_interface;
}
