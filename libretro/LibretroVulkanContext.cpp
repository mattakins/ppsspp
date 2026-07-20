
#include "Common/GPU/Vulkan/VulkanLoader.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanDebug.h"
#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"
#include "GPU/GPUCommon.h"
#include "Common/Data/Text/Parsers.h"

#include "libretro/LibretroVulkanContext.h"
#include <libretro_vulkan.h>
#include <GPU/Vulkan/VulkanRenderManager.h>
#include "ext/glslang/OGLCompilersDLL/InitializeDll.h"

#undef fflush

static VulkanContext *vk;

void vk_libretro_init(VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions, unsigned num_required_device_extensions, const char **required_device_layers, unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features);
void vk_libretro_shutdown();
void vk_libretro_set_hwrender_interface(retro_hw_render_interface *hw_render_interface);
void vk_libretro_wait_for_presentation();

LibretroVulkanContext::LibretroVulkanContext()
	: LibretroHWRenderContext(RETRO_HW_CONTEXT_VULKAN, VK_MAKE_VERSION(1, 0, 18)) {}

void LibretroVulkanContext::SwapBuffers() {
	vk_libretro_wait_for_presentation();
	LibretroHWRenderContext::SwapBuffers();
}

static bool create_device(retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions, unsigned num_required_device_extensions, const char **required_device_layers, unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features) {
	init_glslang();

	VulkanContext *new_vk = new VulkanContext();
	int physical_device = 0;

	vk_libretro_init(instance, gpu, surface, get_instance_proc_addr, required_device_extensions, num_required_device_extensions, required_device_layers, num_required_device_layers, required_features);

	// TODO: Here we'll inject the instance and all of the stuff into the VulkanContext.

	if (new_vk->CreateInstance({}) != VK_SUCCESS)
		goto error;

	while (gpu && new_vk->GetPhysicalDevice(physical_device) != gpu) {
		physical_device++;
	}

	if (!gpu) {
		physical_device = new_vk->GetBestPhysicalDevice();
	}

	if (new_vk->CreateDevice(physical_device) != VK_SUCCESS || new_vk->GetDevice() == VK_NULL_HANDLE)
		goto error;
#ifdef _WIN32
	if (new_vk->InitSurface(WINDOWSYSTEM_WIN32, nullptr, nullptr) != VK_SUCCESS)
		goto error;
#elif defined(__ANDROID__)
	if (new_vk->InitSurface(WINDOWSYSTEM_ANDROID, nullptr, nullptr) != VK_SUCCESS)
		goto error;
#elif defined(VK_USE_PLATFORM_METAL_EXT)
	if (new_vk->InitSurface(WINDOWSYSTEM_METAL_EXT, nullptr, nullptr) != VK_SUCCESS)
		goto error;
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
	if (new_vk->InitSurface(WINDOWSYSTEM_XLIB, nullptr, nullptr) != VK_SUCCESS)
		goto error;
#elif defined(VK_USE_PLATFORM_XCB_KHR)
	if (new_vk->InitSurface(WINDOWSYSTEM_XCB, nullptr, nullptr) != VK_SUCCESS)
		goto error;
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
	if (new_vk->InitSurface(WINDOWSYSTEM_WAYLAND, nullptr, nullptr) != VK_SUCCESS)
		goto error;
#elif defined(VK_USE_PLATFORM_DISPLAY_KHR)
	if (new_vk->InitSurface(WINDOWSYSTEM_DISPLAY, nullptr, nullptr) != VK_SUCCESS)
		goto error;
#endif

	if (new_vk->GetGraphicsQueue() == VK_NULL_HANDLE)
		goto error;

	context->gpu = new_vk->GetPhysicalDevice(physical_device);
	context->device = new_vk->GetDevice();
	context->queue = new_vk->GetGraphicsQueue();
	context->queue_family_index = new_vk->GetGraphicsQueueFamilyIndex();
	context->presentation_queue = context->queue;
	context->presentation_queue_family_index = context->queue_family_index;
	vk = new_vk;
#ifdef _DEBUG
	fflush(stdout);
#endif
	return true;

error:
	ERROR_LOG(Log::G3D, "Failed to create libretro Vulkan device: %s", new_vk->InitError().c_str());
	if (new_vk->GetInstance() != VK_NULL_HANDLE)
	{
		new_vk->DestroySurface();
		if (new_vk->GetDevice() != VK_NULL_HANDLE)
			new_vk->DestroyDevice();
		new_vk->DestroyInstance();
	}
	delete new_vk;
	vk_libretro_shutdown();
	return false;
}

static const VkApplicationInfo *GetApplicationInfo(void) {
	static VkApplicationInfo app_info{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.pApplicationName = "PPSSPP";
	app_info.applicationVersion = Version(PPSSPP_GIT_VERSION).ToInteger();
	app_info.pEngineName = "PPSSPP";
	app_info.engineVersion = 2;
	app_info.apiVersion = VK_API_VERSION_1_0;
	return &app_info;
}

bool LibretroVulkanContext::Init() {
	if (!LibretroHWRenderContext::Init(true)) {
		return false;
	}

	static const struct retro_hw_render_context_negotiation_interface_vulkan iface = {
		RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
		RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
		GetApplicationInfo,
		create_device, // Callback above.
		nullptr,
	};
	Libretro::environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void *)&iface);

	g_Config.iGPUBackend = (int)GPUBackend::VULKAN;
	return true;
}

void LibretroVulkanContext::ContextReset() {
   retro_hw_render_interface *vulkan;
   if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void **)&vulkan) || !vulkan) {
      ERROR_LOG(Log::G3D, "Failed to get HW rendering interface!\n");
      return;
   }
   if (vulkan->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION) {
      ERROR_LOG(Log::G3D, "HW render interface mismatch, expected %u, got %u!\n", RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION, vulkan->interface_version);
      return;
   }
   vk_libretro_set_hwrender_interface(vulkan);

   LibretroHWRenderContext::ContextReset();
}

void LibretroVulkanContext::ContextDestroy() {
   INFO_LOG(Log::G3D, "LibretroVulkanContext::ContextDestroy()");
   if (vk) {
      vk->WaitUntilQueueIdle();
      LibretroHWRenderContext::ContextDestroy();
   }
}

void LibretroVulkanContext::CreateDrawContext() {
   vk->ReinitSurface();

   // TODO: Integrate properly with libretro vulkan context. We currently use a wacky wrapper (libretro_vulkan.cpp)
   if (!vk->InitSwapchain(VK_PRESENT_MODE_FIFO_KHR)) {
      return;
   }

   bool useMultiThreading = g_Config.bRenderMultiThreading;
   if (g_Config.iInflightFrames == 1) {
      useMultiThreading = false;
   }
   draw_ = Draw::T3DCreateVulkanContext(vk, useMultiThreading);

   ((VulkanRenderManager*)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER))->SetInflightFrames(g_Config.iInflightFrames);
   SetGPUBackend(GPUBackend::VULKAN);
}

void LibretroVulkanContext::Shutdown() {
   if (!vk) {
      return;
   }

   if (draw_)
      draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, vk->GetBackbufferWidth(), vk->GetBackbufferHeight());

   LibretroHWRenderContext::Shutdown();

   vk->WaitUntilQueueIdle();

   vk->DestroySwapchain();
   vk->DestroySurface();
   vk->DestroyDevice();
   vk->DestroyInstance();

   delete vk;
   vk = nullptr;

   finalize_glslang();
   glslang::DetachProcess();
   vk_libretro_shutdown();
}

void *LibretroVulkanContext::GetAPIContext() { return vk; }
