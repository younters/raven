#pragma once

namespace raven {
class ApiRegistry;
class NativeObjects;
void register_darwin_d3dkmt(ApiRegistry &registry, NativeObjects &objects);
}
