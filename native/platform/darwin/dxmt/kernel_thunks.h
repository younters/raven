#pragma once
#include <string_view>

namespace raven {
inline bool darwin_keyed_mutex_api(std::string_view name)
{
    return name == "D3DKMTCreateKeyedMutex2" || name == "D3DKMTOpenKeyedMutex2" ||
        name == "D3DKMTAcquireKeyedMutex" || name == "D3DKMTReleaseKeyedMutex" || name == "D3DKMTDestroyKeyedMutex";
}
class ApiRegistry;
class NativeObjects;
class NativeThreads;
void register_darwin_d3dkmt(ApiRegistry &registry, NativeObjects &objects, NativeThreads &threads, bool keyed_only = false);
}
