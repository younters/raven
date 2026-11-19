#pragma once

#include <algorithm>
#include <cstring>
#include <vector>
#include <vulkan/vulkan.h>

// Host reference calls in the upstream tests need the same portability opt-in as the guest bridge.
inline VkResult darwin_test_create_instance(const VkInstanceCreateInfo *source, const VkAllocationCallbacks *allocator, VkInstance *instance)
{
	auto info = *source;
	std::vector<const char *> extensions;
	for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
		extensions.push_back(info.ppEnabledExtensionNames[i]);
	extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	info.enabledExtensionCount = uint32_t(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();
	return vkCreateInstance(&info, allocator, instance);
}

inline VkResult darwin_test_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *source,
	const VkAllocationCallbacks *allocator, VkDevice *device)
{
	auto info = *source;
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> available(count);
	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data());
	std::vector<const char *> extensions;
	for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
		extensions.push_back(info.ppEnabledExtensionNames[i]);
	if (std::any_of(available.begin(), available.end(), [](const auto &item) { return !std::strcmp(item.extensionName, "VK_KHR_portability_subset"); }))
		extensions.push_back("VK_KHR_portability_subset");
	info.enabledExtensionCount = uint32_t(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();
	return vkCreateDevice(physical, &info, allocator, device);
}

#define vkCreateInstance darwin_test_create_instance
#define vkCreateDevice darwin_test_create_device
