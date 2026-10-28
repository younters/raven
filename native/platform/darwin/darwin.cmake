# macOS adjustments to targets native/CMakeLists.txt defines, applied after it so that file stays as upstream wrote it.

if(RAVEN_KOSMICKRISP_ROOT)
    file(READ ${RAVEN_DARWIN_DIR}/graphics/kosmickrisp.json RAVEN_KOSMICKRISP_PIN)
    foreach(component lib/libvulkan_kosmickrisp.dylib share/vulkan/icd.d/libkosmickrisp_icd.json)
        if(NOT EXISTS "${RAVEN_KOSMICKRISP_ROOT}/${component}")
            message(FATAL_ERROR "Missing pinned KosmicKrisp file: ${component}; run scripts/provision-kosmickrisp.py")
        endif()
        string(JSON expected GET "${RAVEN_KOSMICKRISP_PIN}" files "${component}")
        file(SHA256 "${RAVEN_KOSMICKRISP_ROOT}/${component}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "KosmicKrisp file differs from SDK pin: ${component}")
        endif()
    endforeach()
endif()
option(RAVEN_DARWIN_DXVK_TESTS "Register strict DXVK comparison tests, including known unsupported hardware paths" OFF)
if(RAVEN_DARWIN_DXVK_TESTS AND (NOT RAVEN_DXMT_ROOT OR NOT RAVEN_DXVK_PE64_DIR))
    message(FATAL_ERROR "DXVK comparisons need RAVEN_DXMT_ROOT and RAVEN_DXVK_PE64_DIR to build/run matching fixtures")
endif()

# The guest controller: Linux traces an ELF child with ptrace; macOS runs an x86_64 Mach-O agent under Rosetta 2 that the host
# drives through shared memory. Same NativeProcess interface, different implementation file.
if(TARGET raven_cpu)
    include(${RAVEN_DARWIN_DIR}/audio/audio.cmake)
    get_target_property(RAVEN_CPU_SOURCES raven_cpu SOURCES)
    list(REMOVE_ITEM RAVEN_CPU_SOURCES cpu/native_cpu.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/native_process.cpp ${RAVEN_DARWIN_DIR}/unix_calls.cpp)
    list(REMOVE_ITEM RAVEN_CPU_SOURCES desktop/shared_pixels.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/shared_pixels.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/system_api.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/desktop_idle.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/dpi.cpp)
    set_property(TARGET raven_cpu PROPERTY SOURCES ${RAVEN_CPU_SOURCES})
    set(RAVEN_DXMT_ROOT "" CACHE PATH "Provisioned DXMT root (pe64/ and unix64/); RAVEN_DXMT_ROOT environment overrides it")
    if(RAVEN_DXMT_ROOT)
        set_property(SOURCE ${RAVEN_DARWIN_DIR}/unix_calls.cpp APPEND PROPERTY COMPILE_DEFINITIONS RAVEN_DXMT_ROOT="${RAVEN_DXMT_ROOT}")
    endif()
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
    add_test(NAME native_darwin_private_fallback_contracts COMMAND darwin_parallel_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_darwin_private_fallback_contracts PROPERTIES TIMEOUT 30
        ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_DARWIN_DISABLE_PRIVATE_ALIASES=1")
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

if(TARGET raven_cpu AND RAVEN_MINGW64 AND RAVEN_DXMT_ROOT)
    set(RAVEN_DXMT_SHARED ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-shared.exe)
    add_custom_command(OUTPUT ${RAVEN_DXMT_SHARED}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DARWIN_DIR}/tests/dxmt_shared.c
            -o ${RAVEN_DXMT_SHARED} -ld3d11 -ldxgi -ldxguid -lkernel32 -luser32 -lgcc
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/dxmt_shared.c VERBATIM)
    add_custom_target(darwin_dxmt_shared_fixture ALL DEPENDS ${RAVEN_DXMT_SHARED})
    add_test(NAME darwin_dxmt_shared COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
        $<TARGET_FILE:raven> ${RAVEN_DXMT_SHARED} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-shared shared)
    set_tests_properties(darwin_dxmt_shared PROPERTIES TIMEOUT 120)
    set(RAVEN_DXMT_DEVICE10 ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-device10.exe)
    add_custom_command(OUTPUT ${RAVEN_DXMT_DEVICE10}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DARWIN_DIR}/tests/dxmt_device10.c
            -o ${RAVEN_DXMT_DEVICE10} -L${RAVEN_DXMT_ROOT}/pe64 -ld3d10 -ld3d10_1 -ldxgi -ldxguid -lkernel32 -luser32 -lgcc
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/dxmt_device10.c VERBATIM)
    add_custom_target(darwin_dxmt_device10_fixture ALL DEPENDS ${RAVEN_DXMT_DEVICE10})
    add_test(NAME darwin_dxmt_device10 COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
        $<TARGET_FILE:raven> ${RAVEN_DXMT_DEVICE10} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-device10 device10)
    set_tests_properties(darwin_dxmt_device10 PROPERTIES TIMEOUT 120)
    set(RAVEN_DXMT_IMAGE_PYTHON "")
    foreach(candidate ${Python3_EXECUTABLE} /opt/homebrew/bin/python3 /usr/bin/python3)
        if(NOT RAVEN_DXMT_IMAGE_PYTHON AND EXISTS ${candidate})
            execute_process(COMMAND ${candidate} -c "import PIL" RESULT_VARIABLE pillow_result OUTPUT_QUIET ERROR_QUIET)
            if(pillow_result EQUAL 0)
                set(RAVEN_DXMT_IMAGE_PYTHON ${candidate})
            endif()
        endif()
    endforeach()
    if(NOT RAVEN_DXMT_IMAGE_PYTHON)
        message(FATAL_ERROR "DXMT screen tests need a Python interpreter with Pillow")
    endif()
    set(RAVEN_DXMT_WINDOW_INFO ${CMAKE_CURRENT_BINARY_DIR}/dxmt-window-info)
    add_custom_command(OUTPUT ${RAVEN_DXMT_WINDOW_INFO}
        COMMAND /usr/bin/clang -fobjc-arc ${RAVEN_DARWIN_DIR}/tests/window_info.m -framework Foundation -framework CoreGraphics
            -o ${RAVEN_DXMT_WINDOW_INFO}
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/window_info.m VERBATIM)
    add_custom_target(darwin_dxmt_window_info ALL DEPENDS ${RAVEN_DXMT_WINDOW_INFO})
    foreach(workload device pool pacing triangle window deferred geometry tessellation textured msaa compute features fullscreen present_rate)
        if(workload STREQUAL "device")
            set(RAVEN_DXMT_FIXTURE ${RAVEN_DARWIN_DIR}/tests/dxmt_device.c)
        elseif(workload STREQUAL "pool")
            set(RAVEN_DXMT_FIXTURE ${RAVEN_DARWIN_DIR}/tests/dxmt_pool.c)
        elseif(workload STREQUAL "pacing")
            set(RAVEN_DXMT_FIXTURE ${RAVEN_DARWIN_DIR}/tests/dxmt_pacing.c)
        else()
            set(RAVEN_DXMT_FIXTURE ${PROJECT_SOURCE_DIR}/testdata/d3d11/${workload}.c)
        endif()
        set(RAVEN_DXMT_EXE ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-${workload}.exe)
        add_custom_command(OUTPUT ${RAVEN_DXMT_EXE}
            COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DXMT_FIXTURE}
                -o ${RAVEN_DXMT_EXE} -ld3d11 -ldxgi -ldxguid -lkernel32 -luser32 -lgcc
            DEPENDS ${RAVEN_DXMT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/d3d11/tri_shaders.h
                ${PROJECT_SOURCE_DIR}/testdata/d3d11/gs_shaders.h ${PROJECT_SOURCE_DIR}/testdata/d3d11/tess_shaders.h
                ${PROJECT_SOURCE_DIR}/testdata/d3d11/tex_shaders.h ${PROJECT_SOURCE_DIR}/testdata/d3d11/features_shaders.h
                ${PROJECT_SOURCE_DIR}/testdata/d3d11/compute_shader.h VERBATIM)
        add_custom_target(darwin_dxmt_${workload}_fixture ALL DEPENDS ${RAVEN_DXMT_EXE})
        if(workload STREQUAL "window")
            add_test(NAME darwin_dxmt_window COMMAND ${RAVEN_DXMT_IMAGE_PYTHON} ${RAVEN_DARWIN_DIR}/tests/run_present.py
                $<TARGET_FILE:raven> ${RAVEN_DXMT_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-window ${RAVEN_DXMT_WINDOW_INFO})
            set_tests_properties(darwin_dxmt_window PROPERTIES RESOURCE_LOCK raven_compositor)
        else()
            add_test(NAME darwin_dxmt_${workload} COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
                $<TARGET_FILE:raven> ${RAVEN_DXMT_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-${workload} ${workload})
        endif()
        set_tests_properties(darwin_dxmt_${workload} PROPERTIES TIMEOUT 120)
        if(RAVEN_DARWIN_DXVK_TESTS AND workload MATCHES "^(triangle|deferred|geometry|tessellation|textured|msaa|compute|present_rate|pacing)$")
            add_test(NAME darwin_dxvk_${workload} COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
                $<TARGET_FILE:raven> ${RAVEN_DXMT_EXE} ${RAVEN_DXVK_PE64_DIR} ${CMAKE_CURRENT_BINARY_DIR}/dxvk-${workload} ${workload} DXVK)
            set_tests_properties(darwin_dxvk_${workload} PROPERTIES TIMEOUT 120 ENVIRONMENT "VK_DRIVER_FILES=${RAVEN_VULKAN_TEST_ICD}")
            if(workload STREQUAL "present_rate" OR workload STREQUAL "pacing")
                set_tests_properties(darwin_dxvk_${workload} PROPERTIES RESOURCE_LOCK raven_compositor)
            endif()
        endif()
        if(workload STREQUAL "fullscreen" OR workload STREQUAL "present_rate" OR workload STREQUAL "pacing")
            set_tests_properties(darwin_dxmt_${workload} PROPERTIES RESOURCE_LOCK raven_compositor)
        endif()
    endforeach()
    set_tests_properties(darwin_dxmt_device10 PROPERTIES RESOURCE_LOCK raven_compositor)
endif()

# These fixtures use PE DXVK and the host Vulkan bridge; no native WoW64 unix-call table is involved.
if(RAVEN_DARWIN_DXVK_TESTS)
    foreach(width 64 32)
        if(RAVEN_MINGW${width} AND RAVEN_DXVK_PE${width}_DIR)
            foreach(api d3d9 d3d11)
                if(api STREQUAL "d3d9")
                    set(source ${PROJECT_SOURCE_DIR}/testdata/d3d9/window.c)
                    set(entry raven_entry)
                    set(libraries -lkernel32 -luser32)
                    set(kind d3d9)
                elseif(width EQUAL 32)
                    set(source ${PROJECT_SOURCE_DIR}/testdata/d3d11/triangle.c)
                    set(entry _entry@0)
                    set(libraries -ld3d11 -ldxgi -ldxguid -lkernel32 -luser32 -lgcc)
                    set(kind triangle)
                else()
                    continue() # The x64 D3D11 fixture is already registered above.
                endif()
                set(exe ${CMAKE_CURRENT_BINARY_DIR}/darwin-dxvk-${api}-${width}.exe)
                add_custom_command(OUTPUT ${exe}
                    COMMAND ${RAVEN_MINGW${width}} -O2 -nostdlib -fno-builtin -fno-stack-protector
                        -Wl,--entry,${entry},--no-insert-timestamp -o ${exe} ${source} ${libraries}
                    DEPENDS ${source} ${PROJECT_SOURCE_DIR}/testdata/d3d11/tri_shaders.h VERBATIM)
                add_custom_target(darwin_dxvk_${api}_${width}_fixture ALL DEPENDS ${exe})
                add_test(NAME darwin_dxvk_${api}_${width} COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
                    $<TARGET_FILE:raven> ${exe} ${RAVEN_DXVK_PE${width}_DIR} ${CMAKE_CURRENT_BINARY_DIR}/dxvk-${api}-${width} ${kind} DXVK)
                set_tests_properties(darwin_dxvk_${api}_${width} PROPERTIES TIMEOUT 120 ENVIRONMENT "VK_DRIVER_FILES=${RAVEN_VULKAN_TEST_ICD}")
                if(api STREQUAL "d3d9")
                    set_tests_properties(darwin_dxvk_${api}_${width} PROPERTIES RESOURCE_LOCK raven_compositor)
                endif()
            endforeach()
        endif()
    endforeach()
endif()

# A real PE caller and a tiny Mach-O unixlib exercise the boundary without Wine or a graphics driver.
if(TARGET raven_cpu AND RAVEN_MINGW64)
    set(RAVEN_UNIX_CALL_TEST_DIR ${RAVEN_DARWIN_DIR}/tests)
    set(RAVEN_UNIX_CALL_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/raven-unix-call-test.so)
    set(RAVEN_UNIX_CALL_PE ${CMAKE_CURRENT_BINARY_DIR}/raven-unix-call-test.exe)
    add_custom_command(OUTPUT ${RAVEN_UNIX_CALL_LIBRARY}
        COMMAND /usr/bin/clang -arch x86_64 -dynamiclib -O2 -Wall -Wextra
            ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_library.c -o ${RAVEN_UNIX_CALL_LIBRARY}
        DEPENDS ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_library.c ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_shared.h VERBATIM)
    add_custom_command(OUTPUT ${RAVEN_UNIX_CALL_PE}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,mainCRTStartup
            ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_pe.c ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_pe.s -o ${RAVEN_UNIX_CALL_PE}
        DEPENDS ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_pe.c ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_pe.s
            ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_shared.h VERBATIM)
    add_custom_target(darwin_unix_call_fixtures ALL DEPENDS ${RAVEN_UNIX_CALL_LIBRARY} ${RAVEN_UNIX_CALL_PE})
    add_executable(darwin_unix_call_test ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_test.cpp)
    target_link_libraries(darwin_unix_call_test PRIVATE raven_cpu)
    add_dependencies(darwin_unix_call_test darwin_unix_call_fixtures raven-bootstrap)
    add_test(NAME darwin_unix_call COMMAND darwin_unix_call_test $<TARGET_FILE:raven-bootstrap>
        ${RAVEN_UNIX_CALL_PE} ${RAVEN_UNIX_CALL_LIBRARY})
    add_test(NAME darwin_unix_call_parallel COMMAND ${CMAKE_COMMAND} -E env RAVEN_PARALLEL=1
        $<TARGET_FILE:darwin_unix_call_test> $<TARGET_FILE:raven-bootstrap>
        ${RAVEN_UNIX_CALL_PE} ${RAVEN_UNIX_CALL_LIBRARY})
    set_tests_properties(darwin_unix_call darwin_unix_call_parallel PROPERTIES TIMEOUT 20)
endif()

# raven-bootstrap keeps its name, so everything that locates it next to raven finds the macOS guest the same way.
if(TARGET raven-bootstrap)
    set_property(TARGET raven-bootstrap PROPERTY SOURCES ${RAVEN_DARWIN_DIR}/guest/agent.c ${RAVEN_DARWIN_DIR}/guest/reserve.s
        ${RAVEN_DARWIN_DIR}/guest/unix_call.s)
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
    if(test MATCHES "^native_(vulkan_|x86_vulkan_|shared_gpu_memory_|d3d(8|9|11|12)_|x86_d3d)")
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "VK_DRIVER_FILES=${RAVEN_VULKAN_TEST_ICD}")
    endif()
endforeach()

# The macOS desktop (--desktop=macos): AppKit windows, Metal surfaces for graphics drivers, and keymaps from xkbcommon.
if(TARGET raven_cpu)
    enable_language(OBJCXX)
    target_sources(raven_cpu PRIVATE ${RAVEN_DARWIN_DIR}/print.mm)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/print.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    target_link_libraries(raven_cpu PUBLIC "-framework AppKit" "-framework ApplicationServices")
    target_sources(raven_cpu PRIVATE ${RAVEN_DARWIN_DIR}/dxmt/presentation.mm ${RAVEN_DARWIN_DIR}/dxmt/kernel_thunks.mm)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/dxmt/presentation.mm ${RAVEN_DARWIN_DIR}/dxmt/kernel_thunks.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    set_property(TARGET raven_cpu PROPERTY OBJCXX_STANDARD 20)
    target_link_libraries(raven_cpu PUBLIC "-framework IOSurface" "-framework QuartzCore" "-framework Metal")
    target_sources(raven PRIVATE ${RAVEN_DARWIN_DIR}/cocoa.mm ${RAVEN_DARWIN_DIR}/gdi_presenter.mm ${RAVEN_DARWIN_DIR}/app_identity.mm)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/cocoa.mm ${RAVEN_DARWIN_DIR}/gdi_presenter.mm ${RAVEN_DARWIN_DIR}/app_identity.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    set_property(TARGET raven PROPERTY OBJCXX_STANDARD 20)
    target_compile_definitions(raven PRIVATE RAVEN_COCOA=1)
    target_link_options(raven PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${RAVEN_DARWIN_DIR}/runtime-info.plist")
    set_property(TARGET raven APPEND PROPERTY LINK_DEPENDS ${RAVEN_DARWIN_DIR}/runtime-info.plist)
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

if(TARGET native_cpu_test)
    add_executable(darwin_dock_policy_test ${RAVEN_DARWIN_DIR}/dock_policy_test.cpp)
    target_include_directories(darwin_dock_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/native)
    add_test(NAME native_darwin_dock_policy COMMAND darwin_dock_policy_test)
    add_executable(darwin_host_launch_test ${RAVEN_DARWIN_DIR}/host_launch_test.cpp)
    target_link_libraries(darwin_host_launch_test PRIVATE raven_darwin)
    add_test(NAME native_darwin_host_launch COMMAND darwin_host_launch_test)
    add_executable(darwin_foreground_test ${RAVEN_DARWIN_DIR}/foreground_test.mm ${RAVEN_DARWIN_DIR}/app_identity.mm)
    # app_identity.mm is shared by Raven and these contracts and requires ARC in every target.
    set_source_files_properties(${RAVEN_DARWIN_DIR}/app_identity.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    set_source_files_properties(${RAVEN_DARWIN_DIR}/foreground_test.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(darwin_foreground_test PRIVATE ${CMAKE_SOURCE_DIR}/native)
    target_link_libraries(darwin_foreground_test PRIVATE raven_cpu "-framework AppKit")
    add_test(NAME native_darwin_foreground COMMAND darwin_foreground_test)
    set_tests_properties(native_darwin_foreground PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(darwin_cocoa_policy_test ${RAVEN_DARWIN_DIR}/cocoa_policy_test.mm
        ${RAVEN_DARWIN_DIR}/cocoa.mm ${RAVEN_DARWIN_DIR}/gdi_presenter.mm ${RAVEN_DARWIN_DIR}/app_identity.mm)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/cocoa_policy_test.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_property(TARGET darwin_cocoa_policy_test PROPERTY OBJCXX_STANDARD 20)
    target_include_directories(darwin_cocoa_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/native)
    target_link_libraries(darwin_cocoa_policy_test PRIVATE raven_cpu PkgConfig::XKBCOMMON "-framework AppKit" "-framework Carbon" "-framework Metal")
    add_test(NAME native_darwin_cocoa_policy COMMAND darwin_cocoa_policy_test)
    set_tests_properties(native_darwin_cocoa_policy PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(darwin_reexec_test ${RAVEN_DARWIN_DIR}/reexec_test.mm ${RAVEN_DARWIN_DIR}/app_identity.mm)
    target_include_directories(darwin_reexec_test PRIVATE ${CMAKE_SOURCE_DIR}/native)
    target_link_libraries(darwin_reexec_test PRIVATE raven_cpu "-framework AppKit")
    set_property(TARGET darwin_reexec_test PROPERTY OBJCXX_STANDARD 20)
    add_test(NAME native_darwin_reexec COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/reexec.py $<TARGET_FILE:darwin_reexec_test>)
    set_tests_properties(native_darwin_reexec PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(darwin_controller_bench ${RAVEN_DARWIN_DIR}/controller_bench.cpp)
    target_link_libraries(darwin_controller_bench PRIVATE raven_cpu)
    add_executable(darwin_runtime_test ${RAVEN_DARWIN_DIR}/compat_test.cpp)
    target_link_libraries(darwin_runtime_test PRIVATE raven_cpu)
    add_test(NAME native_darwin_runtime COMMAND darwin_runtime_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_darwin_runtime PROPERTIES TIMEOUT 15)
    if(RAVEN_MINGW64)
        set(DARWIN_SYSTEM_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/darwin-system.exe)
        add_custom_command(OUTPUT ${DARWIN_SYSTEM_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib -Wl,--entry,raven_entry,--image-base,0x1c7000000,--no-insert-timestamp
                -o ${DARWIN_SYSTEM_FIXTURE} ${RAVEN_DARWIN_DIR}/system_client.c -lkernel32 -ladvapi32
            DEPENDS ${RAVEN_DARWIN_DIR}/system_client.c VERBATIM)
        add_custom_target(darwin_system_fixture ALL DEPENDS ${DARWIN_SYSTEM_FIXTURE})
        add_test(NAME native_darwin_system_api COMMAND raven --run-process --execution=native ${DARWIN_SYSTEM_FIXTURE})
        set_tests_properties(native_darwin_system_api PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-system-prefix")
    endif()
endif()

if(TARGET raven_cpu AND RAVEN_MINGW64)
    set(CURSOR_DPI_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-aware.exe)
    set(CURSOR_UNAWARE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-unaware.exe)
    set(CURSOR_PM_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-per-monitor.exe)
    set(CURSOR_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-resources/resources.o)
    set(CURSOR_PM_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-resources/manifest.o)
    add_custom_command(OUTPUT ${CURSOR_RESOURCES} ${CURSOR_PM_RESOURCES}
        COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/cursor_dpi_resources.py ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-resources
        COMMAND ${RAVEN_WINDRES} ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-resources/resources.rc ${CURSOR_RESOURCES}
        COMMAND ${RAVEN_WINDRES} -DPER_MONITOR_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-resources/resources.rc ${CURSOR_PM_RESOURCES}
        DEPENDS ${RAVEN_DARWIN_DIR}/cursor_dpi_resources.py VERBATIM)
    foreach(mode IN ITEMS aware unaware per-monitor)
        set(dpi_definitions)
        set(dpi_resources ${CURSOR_RESOURCES})
        if(mode STREQUAL "aware")
            set(dpi_definitions -DAWARE)
        endif()
        if(mode STREQUAL "per-monitor")
            set(dpi_definitions -DAWARE -DPER_MONITOR_MANIFEST)
            set(dpi_resources ${CURSOR_PM_RESOURCES})
        endif()
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-${mode}.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector ${dpi_definitions}
                -Wl,--entry,mainCRTStartup -o ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-${mode}.exe
                ${RAVEN_DARWIN_DIR}/cursor_dpi_fixture.c ${dpi_resources} -lkernel32 -luser32 -lgdi32
            DEPENDS ${RAVEN_DARWIN_DIR}/cursor_dpi_fixture.c ${dpi_resources} VERBATIM)
    endforeach()
    add_custom_target(cursor_dpi_fixtures ALL DEPENDS ${CURSOR_DPI_FIXTURE} ${CURSOR_UNAWARE_FIXTURE} ${CURSOR_PM_FIXTURE})
    add_executable(cursor_dpi_test ${RAVEN_DARWIN_DIR}/cursor_dpi_test.cpp)
    target_link_libraries(cursor_dpi_test PRIVATE raven_cpu)
    add_dependencies(cursor_dpi_test cursor_dpi_fixtures raven-bootstrap)
    add_test(NAME native_darwin_cursor_dpi COMMAND cursor_dpi_test $<TARGET_FILE:raven-bootstrap> ${CURSOR_DPI_FIXTURE} ${CURSOR_UNAWARE_FIXTURE} ${CURSOR_PM_FIXTURE})
endif()

if(TARGET raven_cpu)
    add_executable(darwin_gdi_presenter_test ${RAVEN_DARWIN_DIR}/gdi_presenter_test.mm ${RAVEN_DARWIN_DIR}/gdi_presenter.mm)
    target_link_libraries(darwin_gdi_presenter_test PRIVATE raven_cpu "-framework Metal")
    set_property(TARGET darwin_gdi_presenter_test PROPERTY OBJCXX_STANDARD 20)
    add_test(NAME native_darwin_gdi_presenter COMMAND darwin_gdi_presenter_test)
endif()

# Host-only shortcut parser and generated bundle ownership/lifecycle contracts.
add_test(NAME native_darwin_apps COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/apps_test.py)

# Both guest widths exercise the same printer contracts and verify the resulting PDF on the host.
if(TARGET raven AND RAVEN_MINGW64)
    foreach(width 64 32)
        if(width EQUAL 64)
            set(PRINT_COMPILER ${RAVEN_MINGW64})
        else()
            set(PRINT_COMPILER ${RAVEN_MINGW32})
        endif()
        if(PRINT_COMPILER)
            set(PRINT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/printing${width}.exe)
            add_custom_command(OUTPUT ${PRINT_FIXTURE}
                COMMAND ${PRINT_COMPILER} -O2 -nostdlib -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry -o ${PRINT_FIXTURE}
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/printing_client.c -luser32 -lgdi32 -lwinspool -lcomdlg32 -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/printing_client.c VERBATIM)
            add_custom_target(printing_fixture${width} ALL DEPENDS ${PRINT_FIXTURE})
            add_test(NAME native_printing_pdf${width} COMMAND ${Python3_EXECUTABLE}
                ${RAVEN_DARWIN_DIR}/tests/printing.py $<TARGET_FILE:raven> ${PRINT_FIXTURE}
                ${CMAKE_CURRENT_BINARY_DIR}/printing${width})
            set_tests_properties(native_printing_pdf${width} PROPERTIES TIMEOUT 120)
        endif()
    endforeach()
endif()

# Worker processes retain their creator's desktop pixels even with a different DPI declaration.
if(TARGET raven AND RAVEN_MINGW64)
    foreach(width 64 32)
        if(width EQUAL 64)
            set(dpi_compiler ${RAVEN_MINGW64})
            set(dpi_entry raven_entry)
        else()
            set(dpi_compiler ${RAVEN_MINGW32})
            set(dpi_entry _raven_entry)
        endif()
        if(dpi_compiler)
            set(dpi_client ${CMAKE_CURRENT_BINARY_DIR}/desktop-dpi${width}.exe)
            add_custom_command(OUTPUT ${dpi_client}
                COMMAND ${dpi_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,${dpi_entry},--no-insert-timestamp -o ${dpi_client}
                    ${RAVEN_DARWIN_DIR}/tests/desktop_dpi.c -luser32 -lgdi32 -lkernel32
                DEPENDS ${RAVEN_DARWIN_DIR}/tests/desktop_dpi.c VERBATIM)
            add_custom_target(darwin_desktop_dpi${width}_fixture ALL DEPENDS ${dpi_client})
            add_test(NAME native_darwin_desktop_dpi_x${width} COMMAND raven --run-process --execution=native --desktop=macos ${dpi_client})
            set_tests_properties(native_darwin_desktop_dpi_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/desktop-dpi-prefix)
        endif()
    endforeach()
endif()

# The upstream D3D10.1 fixture expects its fallback component to report unsupported hardware. Keep DXMT's
# hardware path for applications and exercise it separately with darwin_dxmt_device10.
if(TEST native_d3d10_1_x64)
    set_tests_properties(native_d3d10_1_x64 PROPERTIES ENVIRONMENT "RAVEN_DXMT_ROOT=")
endif()
