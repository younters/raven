# macOS adjustments to targets native/CMakeLists.txt defines, applied after it so that file stays as upstream wrote it.

# The guest controller: Linux traces an ELF child with ptrace; macOS runs an x86_64 Mach-O agent under Rosetta 2 that the host
# drives through shared memory. Same NativeProcess interface, different implementation file.
if(TARGET raven_cpu)
    get_target_property(RAVEN_CPU_SOURCES raven_cpu SOURCES)
    list(REMOVE_ITEM RAVEN_CPU_SOURCES cpu/native_cpu.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/native_process.cpp)
    list(REMOVE_ITEM RAVEN_CPU_SOURCES desktop/shared_pixels.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/shared_pixels.cpp)
    set_property(TARGET raven_cpu PROPERTY SOURCES ${RAVEN_CPU_SOURCES})
    # GNU ld only; ld64 keeps every library it is given.
    get_target_property(RAVEN_CPU_LINK_OPTIONS raven_cpu INTERFACE_LINK_OPTIONS)
    if(RAVEN_CPU_LINK_OPTIONS)
        list(REMOVE_ITEM RAVEN_CPU_LINK_OPTIONS "LINKER:--no-as-needed")
        set_property(TARGET raven_cpu PROPERTY INTERFACE_LINK_OPTIONS ${RAVEN_CPU_LINK_OPTIONS})
    endif()
    add_executable(darwin_parallel_test ${RAVEN_DARWIN_DIR}/parallel_test.cpp)
    target_link_libraries(darwin_parallel_test PRIVATE raven_cpu)
    add_dependencies(darwin_parallel_test raven-bootstrap)
    add_test(NAME native_darwin_parallel_contracts COMMAND darwin_parallel_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_darwin_parallel_contracts PROPERTIES TIMEOUT 30 ENVIRONMENT RAVEN_PARALLEL=1)
    if(RAVEN_MINGW64)
        set(RAVEN_DARWIN_PARALLEL_BENCHMARK ${CMAKE_CURRENT_BINARY_DIR}/darwin-parallel-benchmark.exe)
        add_custom_command(OUTPUT ${RAVEN_DARWIN_PARALLEL_BENCHMARK}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x199a00000
                -o ${RAVEN_DARWIN_PARALLEL_BENCHMARK} ${RAVEN_DARWIN_DIR}/parallel_benchmark.c -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/parallel_benchmark.c VERBATIM)
        add_custom_target(darwin_parallel_benchmark ALL DEPENDS ${RAVEN_DARWIN_PARALLEL_BENCHMARK})
    endif()
endif()

# A Windows Vulkan program presents into the actual CAMetalLayer, then replaces its swapchain after a client resize.
if(TARGET vulkan_pe_fixture AND RAVEN_MINGW64)
    set(RAVEN_COCOA_VULKAN_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/vulkan-fixtures/cocoa-present.exe)
    add_custom_command(OUTPUT ${RAVEN_COCOA_VULKAN_FIXTURE}
        COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -mno-stack-arg-probe
            -idirafter ${Vulkan_INCLUDE_DIRS} -Wl,--entry,raven_entry
            -o ${RAVEN_COCOA_VULKAN_FIXTURE} ${RAVEN_DARWIN_DIR}/tests/vulkan_present.c
            ${VULKAN_FIXTURES}/vulkan.a -lkernel32 -luser32
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/vulkan_present.c ${VULKAN_FIXTURES}/vulkan.a VERBATIM)
    add_custom_target(cocoa_vulkan_fixture ALL DEPENDS ${RAVEN_COCOA_VULKAN_FIXTURE})
    # This test needs a graphical login and screen capture access, so opt in when running desktop integration tests.
    option(RAVEN_DARWIN_PRESENT_TEST "Test Vulkan presentation on the logged-in macOS desktop (requires Pillow)" OFF)
    if(RAVEN_DARWIN_PRESENT_TEST)
        find_program(RAVEN_DARWIN_PRESENT_PYTHON NAMES python3 REQUIRED)
        add_test(NAME native_vulkan_cocoa_present COMMAND ${RAVEN_DARWIN_PRESENT_PYTHON}
            ${RAVEN_DARWIN_DIR}/tests/vulkan_present.py $<TARGET_FILE:raven> ${RAVEN_COCOA_VULKAN_FIXTURE}
            ${CMAKE_CURRENT_BINARY_DIR}/vulkan-cocoa-present)
        set_tests_properties(native_vulkan_cocoa_present PROPERTIES TIMEOUT 60)
    endif()
endif()

# raven-bootstrap keeps its name, so everything that locates it next to raven finds the macOS guest the same way.
if(TARGET raven-bootstrap)
    set_property(TARGET raven-bootstrap PROPERTY SOURCES ${RAVEN_DARWIN_DIR}/guest/agent.c ${RAVEN_DARWIN_DIR}/guest/reserve.s)
    set_property(TARGET raven-bootstrap PROPERTY COMPILE_OPTIONS -O2 -Wall -Wextra -fno-sanitize=all -fno-stack-protector)
    set_property(TARGET raven-bootstrap PROPERTY LINKER_LANGUAGE C)
    set_property(TARGET raven-bootstrap PROPERTY OSX_ARCHITECTURES x86_64)
    # A guest program: none of the host's Linux shims or libraries.
    set_property(TARGET raven-bootstrap PROPERTY INCLUDE_DIRECTORIES "")
    set_property(TARGET raven-bootstrap PROPERTY LINK_LIBRARIES "")
    # Windows images want the low 4 GiB: the agent loads high and a zero-fill segment holds the low range until the host maps it.
    set_property(TARGET raven-bootstrap PROPERTY LINK_OPTIONS -fno-sanitize=all -Wl,-no_pie -Wl,-pagezero_size,0x10000
        -Wl,-image_base,0x700000000000 -Wl,-segaddr,RAVEN_LOW,0x10000 -Wl,-w)
endif()

# The Linux interprocess behaviour of the shims: sequenced packets, abstract socket names and the /proc process files.
add_executable(darwin_compat_test ${RAVEN_DARWIN_DIR}/compat/compat_test.cpp)
add_test(NAME darwin_compat_contract COMMAND darwin_compat_test)

# The Vulkan loader of the LunarG SDK names itself @rpath/libvulkan.1.dylib.
if(TARGET Vulkan::Vulkan AND Vulkan_LIBRARY)
    get_filename_component(RAVEN_VULKAN_LIBRARY_DIRECTORY ${Vulkan_LIBRARY} DIRECTORY)
    set_property(TARGET Vulkan::Vulkan APPEND PROPERTY INTERFACE_LINK_OPTIONS "LINKER:-rpath,${RAVEN_VULKAN_LIBRARY_DIRECTORY}")
endif()

# These exercise capabilities the Darwin agent / MoltenVK do not implement. Keep the distinction visible in CTest.
if(TEST native_vulkan_queued_extensions_x64)
    set_tests_properties(native_vulkan_queued_extensions_x64 PROPERTIES DISABLED TRUE) # Requires parallel guest threads.
endif()
foreach(test native_vulkan_descriptor_buffer_x64 native_vulkan_descriptor_buffer_x32)
    if(TEST ${test})
        set_tests_properties(${test} PROPERTIES SKIP_REGULAR_EXPRESSION "Native entry returned 3 ") # Extension not offered.
    endif()
endforeach()

# Use the SDK ICD consistently, including host reference calls made outside the bridge.
get_property(RAVEN_DARWIN_TESTS DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS RAVEN_DARWIN_TESTS)
    if(test MATCHES "^native_(vulkan_|x86_vulkan_|shared_gpu_memory_)")
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "VK_DRIVER_FILES=${RAVEN_VULKAN_TEST_ICD}")
    endif()
endforeach()

# The macOS desktop (--desktop=macos): AppKit windows, Metal surfaces for graphics drivers, and keymaps from xkbcommon.
if(TARGET raven_cpu)
    enable_language(OBJCXX)
    target_sources(raven PRIVATE ${RAVEN_DARWIN_DIR}/cocoa.mm)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/cocoa.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    set_property(TARGET raven PROPERTY OBJCXX_STANDARD 20)
    target_compile_definitions(raven PRIVATE RAVEN_COCOA=1)
    target_link_libraries(raven PRIVATE "-framework AppKit" "-framework QuartzCore" "-framework Carbon" "-framework Metal")
    if(TARGET PkgConfig::XKBCOMMON)
        target_link_libraries(raven PRIVATE PkgConfig::XKBCOMMON)
    else()
        message(WARNING "xkbcommon is missing: the macOS desktop needs it for keyboard input (brew install libxkbcommon)")
    endif()
endif()

# Skia's macOS build always carries its CoreGraphics image generator.
if(TARGET raven_text)
    target_link_libraries(raven_text PUBLIC "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText" "-framework ImageIO")
endif()
