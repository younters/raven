/* A Windows Vulkan client for the Cocoa integration test. Each quadrant is a render-pass clear, presented through a real swapchain. */
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcA(window, message, wparam, lparam);
}

static void pump(void)
{
	MSG message;
	while (PeekMessageA(&message, 0, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageA(&message);
	}
}

static int checkpoint(const char *text, const char *next)
{
	HANDLE file = CreateFileA("C:\\vulkan-present.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	DWORD written;
	CHECK(file != INVALID_HANDLE_VALUE && WriteFile(file, text, lstrlenA(text), &written, 0));
	CloseHandle(file);
	for (unsigned i = 0; i < 1500; ++i) {
		pump();
		if (GetFileAttributesA(next) != INVALID_FILE_ATTRIBUTES) return 0;
		Sleep(10);
	}
	return __LINE__;
}

unsigned int raven_entry(void)
{
	WNDCLASSA klass = {0};
	klass.lpfnWndProc = procedure;
	klass.lpszClassName = "RavenVulkanPresent";
	CHECK(RegisterClassA(&klass));
	RECT rectangle = {0, 0, 256, 192};
	CHECK(AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE));
	HWND window = CreateWindowA(klass.lpszClassName, "Raven MoltenVK presentation", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		100, 100, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, 0, 0, 0, 0);
	CHECK(window);
	pump();
	const char *extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
	VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "Cocoa presentation fixture", 1, 0, 0, VK_API_VERSION_1_1};
	VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &application, 0, 0, 2, extensions};
	VkInstance instance;
	CHECK(vkCreateInstance(&instance_info, 0, &instance) == VK_SUCCESS);
	PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR");
	VkWin32SurfaceCreateInfoKHR surface_info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, 0, window};
	VkSurfaceKHR surface;
	CHECK(create && create(instance, &surface_info, 0, &surface) == VK_SUCCESS);
	VkPhysicalDevice physical[16];
	uint32_t count = 16;
	CHECK(vkEnumeratePhysicalDevices(instance, &count, physical) == VK_SUCCESS && count);
	VkBool32 supported = VK_FALSE;
	CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physical[0], 0, surface, &supported) == VK_SUCCESS && supported);
	VkSurfaceFormatKHR formats[256];
	count = 256;
	CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical[0], surface, &count, formats) == VK_SUCCESS);
	uint32_t format = 0;
	while (format < count && formats[format].format != VK_FORMAT_B8G8R8A8_UNORM) ++format;
	CHECK(format < count);
	float priority = 1;
	VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &priority};
	const char *swap_extension = "VK_KHR_swapchain";
	VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &queue_info, 0, 0, 1, &swap_extension};
	VkDevice device;
	CHECK(vkCreateDevice(physical[0], &device_info, 0, &device) == VK_SUCCESS);
	VkQueue queue;
	vkGetDeviceQueue(device, 0, 0, &queue);
	VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(device, &pool_info, 0, &pool) == VK_SUCCESS);
	VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer command;
	CHECK(vkAllocateCommandBuffers(device, &allocate, &command) == VK_SUCCESS);
	VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore acquired, finished;
	CHECK(vkCreateSemaphore(device, &semaphore_info, 0, &acquired) == VK_SUCCESS &&
		vkCreateSemaphore(device, &semaphore_info, 0, &finished) == VK_SUCCESS);
	VkAttachmentDescription attachment = {0, formats[format].format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
	VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &reference};
	VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &attachment, 1, &subpass};
	VkRenderPass pass;
	CHECK(vkCreateRenderPass(device, &pass_info, 0, &pass) == VK_SUCCESS);
	for (unsigned phase = 0; phase < 2; ++phase) {
		uint32_t width = phase ? 320 : 256, height = phase ? 200 : 192;
		if (phase) {
			rectangle = (RECT){0, 0, width, height};
			CHECK(AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE));
			CHECK(SetWindowPos(window, 0, 0, 0, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, SWP_NOMOVE | SWP_NOZORDER));
			pump();
		}
		VkSurfaceCapabilitiesKHR caps;
		CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical[0], surface, &caps) == VK_SUCCESS &&
			caps.currentExtent.width == width && caps.currentExtent.height == height);
		VkSwapchainCreateInfoKHR swap_info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0, 0, surface, caps.minImageCount,
			formats[format].format, formats[format].colorSpace, {width, height}, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			VK_SHARING_MODE_EXCLUSIVE, 0, 0, caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE};
		VkSwapchainKHR swap;
		CHECK(vkCreateSwapchainKHR(device, &swap_info, 0, &swap) == VK_SUCCESS);
		VkImage images[16];
		count = 16;
		CHECK(vkGetSwapchainImagesKHR(device, swap, &count, images) == VK_SUCCESS);
		for (unsigned frame = 0; frame < 3; ++frame) {
			uint32_t index;
			CHECK(vkAcquireNextImageKHR(device, swap, UINT64_MAX, acquired, 0, &index) == VK_SUCCESS);
			VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, images[index], VK_IMAGE_VIEW_TYPE_2D,
				formats[format].format, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
			VkImageView view;
			CHECK(vkCreateImageView(device, &view_info, 0, &view) == VK_SUCCESS);
			VkFramebufferCreateInfo framebuffer_info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, 0, 0, pass, 1, &view, width, height, 1};
			VkFramebuffer framebuffer;
			CHECK(vkCreateFramebuffer(device, &framebuffer_info, 0, &framebuffer) == VK_SUCCESS);
			CHECK(vkResetCommandBuffer(command, 0) == VK_SUCCESS);
			VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			CHECK(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
			VkRenderPassBeginInfo render = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, pass, framebuffer, {{0, 0}, {width, height}}};
			vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
			for (unsigned quadrant = 0; quadrant < 4; ++quadrant) {
				VkClearAttachment clear = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0, 0, 0, 1}}}};
				unsigned color = (quadrant + phase) % 4;
				if (color == 3) clear.clearValue.color.float32[0] = clear.clearValue.color.float32[1] = clear.clearValue.color.float32[2] = 1;
				else clear.clearValue.color.float32[color] = 1;
				VkClearRect rect = {{{(quadrant % 2) * width / 2, (quadrant / 2) * height / 2}, {width / 2, height / 2}}, 0, 1};
				vkCmdClearAttachments(command, 1, &clear, 1, &rect);
			}
			vkCmdEndRenderPass(command);
			CHECK(vkEndCommandBuffer(command) == VK_SUCCESS);
			VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 1, &acquired, &stage, 1, &command, 1, &finished};
			CHECK(vkQueueSubmit(queue, 1, &submit, 0) == VK_SUCCESS);
			VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &finished, 1, &swap, &index};
			CHECK(vkQueuePresentKHR(queue, &present) == VK_SUCCESS && vkQueueWaitIdle(queue) == VK_SUCCESS);
			vkDestroyFramebuffer(device, framebuffer, 0);
			vkDestroyImageView(device, view, 0);
			pump();
		}
		CHECK(checkpoint(phase ? "resized" : "first", phase ? "C:\\vulkan-present-done" : "C:\\vulkan-present-next") == 0);
		vkDestroySwapchainKHR(device, swap, 0);
	}
	vkDestroyRenderPass(device, pass, 0);
	vkDestroySemaphore(device, finished, 0);
	vkDestroySemaphore(device, acquired, 0);
	vkDestroyCommandPool(device, pool, 0);
	vkDestroyDevice(device, 0);
	vkDestroySurfaceKHR(instance, surface, 0);
	vkDestroyInstance(instance, 0);
	DestroyWindow(window);
	return 0;
}
