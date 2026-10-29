/* Vulkan initialization may probe a surface without ever showing its HWND. */
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#define CHECK(value) do { if (!(value)) return __LINE__; } while (0)

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	WNDCLASSW klass = {0};
	klass.lpfnWndProc = procedure;
	klass.lpszClassName = L"RavenHiddenVulkanSurface";
	CHECK(RegisterClassW(&klass));
	const HWND foreground = GetForegroundWindow();
	HWND window = CreateWindowExW(WS_EX_NOACTIVATE, klass.lpszClassName, L"Hidden Vulkan surface", WS_POPUP,
		0, 0, 1, 1, NULL, NULL, NULL, NULL);
	CHECK(window && !IsWindowVisible(window));
	const char *extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
	VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "Hidden surface", 1, NULL, 0, VK_API_VERSION_1_0};
	VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &application, 0, NULL, 2, extensions};
	VkInstance instance;
	CHECK(vkCreateInstance(&info, NULL, &instance) == VK_SUCCESS);
	PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR");
	CHECK(create);
	for (unsigned iteration = 0; iteration < 2; ++iteration) {
		VkWin32SurfaceCreateInfoKHR surface_info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0, NULL, window};
		VkSurfaceKHR surface;
		CHECK(create(instance, &surface_info, NULL, &surface) == VK_SUCCESS);
		CHECK(!IsWindowVisible(window) && GetForegroundWindow() == foreground);
		vkDestroySurfaceKHR(instance, surface, NULL);
	}
	CHECK(DestroyWindow(window));
	vkDestroyInstance(instance, NULL);
	return 0;
}
