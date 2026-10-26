#pragma once
#include <cstdint>
#include <string_view>
#include <span>
#include <vector>
namespace raven { class ApiRegistry; class ApiCall; struct KernelContext; }
namespace raven_darwin {
std::span<const std::string_view> audio_method_names();
uint32_t audio_method_words(std::string_view name);
void register_audio_api(raven::ApiRegistry &, raven::KernelContext &);
bool create_audio_instance(raven::ApiCall &);
void register_playsound_api(raven::ApiRegistry &, raven::KernelContext &);
}
