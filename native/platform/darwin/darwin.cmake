# macOS adjustments to targets native/CMakeLists.txt defines, applied after it so that file stays as upstream wrote it.

# The guest controller: Linux traces an ELF child with ptrace; macOS runs an x86_64 Mach-O agent under Rosetta 2 that the host
# drives through shared memory. Same NativeProcess interface, different implementation file.
if(TARGET raven_cpu)
    get_target_property(RAVEN_CPU_SOURCES raven_cpu SOURCES)
    list(REMOVE_ITEM RAVEN_CPU_SOURCES cpu/native_cpu.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/native_process.cpp)
    set_property(TARGET raven_cpu PROPERTY SOURCES ${RAVEN_CPU_SOURCES})
    # GNU ld only; ld64 keeps every library it is given.
    get_target_property(RAVEN_CPU_LINK_OPTIONS raven_cpu INTERFACE_LINK_OPTIONS)
    if(RAVEN_CPU_LINK_OPTIONS)
        list(REMOVE_ITEM RAVEN_CPU_LINK_OPTIONS "LINKER:--no-as-needed")
        set_property(TARGET raven_cpu PROPERTY INTERFACE_LINK_OPTIONS ${RAVEN_CPU_LINK_OPTIONS})
    endif()
endif()

# raven-bootstrap keeps its name, so everything that locates it next to raven finds the macOS guest the same way.
if(TARGET raven-bootstrap)
    set_property(TARGET raven-bootstrap PROPERTY SOURCES ${RAVEN_DARWIN_DIR}/guest/agent.c ${RAVEN_DARWIN_DIR}/guest/reserve.s)
    set_property(TARGET raven-bootstrap PROPERTY LINKER_LANGUAGE C)
    set_property(TARGET raven-bootstrap PROPERTY OSX_ARCHITECTURES x86_64)
    set_property(TARGET raven-bootstrap PROPERTY COMPILE_OPTIONS -fno-sanitize=all -fno-stack-protector)
    # A guest program: none of the host's Linux shims or libraries.
    set_property(TARGET raven-bootstrap PROPERTY INCLUDE_DIRECTORIES "")
    set_property(TARGET raven-bootstrap PROPERTY LINK_LIBRARIES "")
    # Windows images want the low 4 GiB: the agent loads high and a zero-fill segment holds the low range until the host maps it.
    set_property(TARGET raven-bootstrap PROPERTY LINK_OPTIONS -fno-sanitize=all -Wl,-no_pie -Wl,-pagezero_size,0x10000
        -Wl,-image_base,0x7f0000000000 -Wl,-segaddr,RAVEN_LOW,0x10000 -Wl,-w)
endif()

# The Vulkan loader of the LunarG SDK names itself @rpath/libvulkan.1.dylib.
if(TARGET Vulkan::Vulkan AND Vulkan_LIBRARY)
    get_filename_component(RAVEN_VULKAN_LIBRARY_DIRECTORY ${Vulkan_LIBRARY} DIRECTORY)
    set_property(TARGET raven_cpu APPEND PROPERTY INTERFACE_LINK_OPTIONS "LINKER:-rpath,${RAVEN_VULKAN_LIBRARY_DIRECTORY}")
endif()
