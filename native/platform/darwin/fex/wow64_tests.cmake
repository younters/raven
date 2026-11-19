# SPDX-License-Identifier: MIT
# Included only by the opt-in module-host subtree. Shared Darwin defaults live elsewhere.
if(NOT RAVEN_MINGW32)
	message(WARNING "FEX WoW64 tests unavailable: set RAVEN_MINGW32 to an i686 MinGW compiler")
	return()
endif()
if(NOT TARGET raven_cpu)
	return()
endif()
# The WoW64 tests need the provisioned 32-bit translator. Without it they are not registered, so a plain build's ctest does not fail;
# a qualification build sets RAVEN_FEX_REQUIRE_MODULES so that a missing module stops the configure instead of skipping silently.
if(NOT EXISTS "${RAVEN_FEX_MODULE_ROOT}/xtajit.dll")
	if(RAVEN_FEX_REQUIRE_MODULES)
		message(FATAL_ERROR "RAVEN_FEX_REQUIRE_MODULES is set but ${RAVEN_FEX_MODULE_ROOT}/xtajit.dll is missing")
	endif()
	message(STATUS "FEX WoW64 tests not registered: provision RAVEN_FEX_MODULE_ROOT/xtajit.dll (native/platform/darwin/fex/scripts/build-fex-modules.py)")
	return()
endif()
set(WOW64_ENTRY ${CMAKE_BINARY_DIR}/native/fex-entry32.exe)
add_custom_command(OUTPUT ${WOW64_ENTRY}
	COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
		-Wl,--entry,_raven_entry,--image-base,0x400000,--no-insert-timestamp
		-o ${WOW64_ENTRY} ${RAVEN_MODHOST_REPO}/testdata/native_cpu/entry.c
	DEPENDS ${RAVEN_MODHOST_REPO}/testdata/native_cpu/entry.c VERBATIM)
set(WOW64_OUTPUTS ${WOW64_ENTRY})
add_test(NAME native_entry32_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-wow64.py
	--raven $<TARGET_FILE:raven> --fixture ${WOW64_ENTRY} --modules "${RAVEN_FEX_MODULE_ROOT}" --entry)
set_tests_properties(native_entry32_fex PROPERTIES TIMEOUT 30 LABELS "fex;wow64;entry")
foreach(client ansi file threads)
	set(fixture ${CMAKE_BINARY_DIR}/native/kernel-${client}-fex32.exe)
	set(source ${RAVEN_MODHOST_REPO}/testdata/native_loader/kernel_${client}_client.c)
	if(client STREQUAL "file")
		set(source ${RAVEN_MODHOST_REPO}/testdata/native_loader/kernel_files_client.c)
	endif()
	add_custom_command(OUTPUT ${fixture}
		COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
			-Wl,--entry,_raven_entry,--image-base,0x400000,--no-insert-timestamp
			-o ${fixture} ${source} -lkernel32
		DEPENDS ${source} VERBATIM)
	list(APPEND WOW64_OUTPUTS ${fixture})
	add_test(NAME native_kernel_${client}32_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-wow64.py
		--raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
	set_tests_properties(native_kernel_${client}32_fex PROPERTIES TIMEOUT 30 LABELS "fex;wow64;process")
endforeach()
add_custom_target(fex_wow64_fixtures DEPENDS ${WOW64_OUTPUTS})
add_executable(fex_wow64_process_test ${CMAKE_CURRENT_LIST_DIR}/wow64_process_test.cpp)
target_link_libraries(fex_wow64_process_test PRIVATE raven_cpu)
# x18 holds the execution TEB; the macOS 11 legacy SDK permits its use.
target_link_options(fex_wow64_process_test PRIVATE -Wl,-platform_version,macos,11.0,11.0)
add_dependencies(fex_wow64_process_test fex_wow64_fixtures)
foreach(contract unaligned fs fault timeout host-smc guest-smc stdcall cdecl)
	add_test(NAME native_wow64_${contract}_fex COMMAND fex_wow64_process_test ${WOW64_ENTRY} ${contract})
	set_tests_properties(native_wow64_${contract}_fex PROPERTIES TIMEOUT 15 LABELS "fex;wow64;engine"
		ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
endforeach()
set(WOW64_SEH ${CMAKE_BINARY_DIR}/native/kernel-seh-fex32.exe)
add_custom_command(OUTPUT ${WOW64_SEH}
	COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
		-Wl,--entry,_raven_entry,--image-base,0x400000,--no-insert-timestamp
		-o ${WOW64_SEH} ${CMAKE_CURRENT_LIST_DIR}/wow64_seh_client.c -lkernel32
	DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_seh_client.c VERBATIM)
add_custom_target(fex_wow64_seh_fixture DEPENDS ${WOW64_SEH})
add_test(NAME native_kernel_seh32_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-wow64.py
	--raven $<TARGET_FILE:raven> --fixture ${WOW64_SEH} --modules "${RAVEN_FEX_MODULE_ROOT}")
set_tests_properties(native_kernel_seh32_fex PROPERTIES TIMEOUT 30 LABELS "fex;wow64;process;seh")
add_test(NAME native_kernel_threads32_parallel_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-wow64.py
	--raven $<TARGET_FILE:raven> --fixture ${CMAKE_BINARY_DIR}/native/kernel-threads-fex32.exe --modules "${RAVEN_FEX_MODULE_ROOT}" --parallel)
set_tests_properties(native_kernel_threads32_parallel_fex PROPERTIES TIMEOUT 30 LABELS "fex;wow64;process;parallel")
add_dependencies(fex_wow64_process_test fex_wow64_seh_fixture)
foreach(contract fault timeout host-smc guest-smc)
	add_test(NAME native_wow64_${contract}_parallel_fex COMMAND fex_wow64_process_test ${WOW64_ENTRY} ${contract})
	set_tests_properties(native_wow64_${contract}_parallel_fex PROPERTIES TIMEOUT 15 LABELS "fex;wow64;engine;parallel"
		ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT};RAVEN_PARALLEL=1;RAVEN_SLICE_MS=0")
endforeach()

add_test(NAME native_wow64_native_brk_rejection_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-rejection.py
	--host $<TARGET_FILE:fex_wow64_process_test> --fixture ${WOW64_ENTRY} --modules "${RAVEN_FEX_MODULE_ROOT}" --wow64)
set_tests_properties(native_wow64_native_brk_rejection_fex PROPERTIES TIMEOUT 15 LABELS "fex;wow64;fault")
add_test(NAME native_nt_service_fex COMMAND fex_process_test ${CMAKE_BINARY_DIR}/native/fex-entry.exe nt-service)
set_tests_properties(native_nt_service_fex PROPERTIES TIMEOUT 15 LABELS "fex;wow64;contract"
	ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")

set(WOW64_ALIGN ${CMAKE_BINARY_DIR}/native/fex-alignment32.exe)
set(WOW64_CRT ${CMAKE_BINARY_DIR}/native/fex-crt32.exe)
if(RAVEN_DLLTOOL32)
	set(WOW64_DLLTOOL ${RAVEN_DLLTOOL32})
else()
	find_program(WOW64_DLLTOOL NAMES i686-w64-mingw32-dlltool llvm-dlltool REQUIRED)
endif()
set(WOW64_NTDLL_IMPORT ${CMAKE_BINARY_DIR}/native/fex-ntdll32.a)
add_custom_command(OUTPUT ${WOW64_NTDLL_IMPORT}
	COMMAND ${WOW64_DLLTOOL} -m i386 -d ${CMAKE_CURRENT_LIST_DIR}/wow64_crt_ntdll.def -l ${WOW64_NTDLL_IMPORT}
	DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_crt_ntdll.def VERBATIM)
add_custom_command(OUTPUT ${WOW64_ALIGN}
	COMMAND ${RAVEN_MINGW32} -nostdlib -Wl,--entry,_raven_entry,--no-insert-timestamp
		-o ${WOW64_ALIGN} ${CMAKE_CURRENT_LIST_DIR}/wow64_alignment.S
	DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_alignment.S VERBATIM)
add_custom_command(OUTPUT ${WOW64_CRT}
	COMMAND ${RAVEN_MINGW32} -nostdlib -Wl,--entry,_raven_entry,--no-insert-timestamp
		-o ${WOW64_CRT} ${CMAKE_CURRENT_LIST_DIR}/wow64_crt.S ${WOW64_NTDLL_IMPORT}
	DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_crt.S ${WOW64_NTDLL_IMPORT} VERBATIM)
add_custom_target(fex_wow64_alignment_crt_fixtures DEPENDS ${WOW64_ALIGN} ${WOW64_CRT})
add_dependencies(fex_wow64_process_test fex_wow64_alignment_crt_fixtures)
foreach(contract alignment range range-laa)
	set(fixture ${WOW64_ENTRY})
	if(contract STREQUAL "alignment")
		set(fixture ${WOW64_ALIGN})
	endif()
	add_test(NAME native_wow64_${contract}_fex COMMAND fex_wow64_process_test ${fixture} ${contract})
	set_tests_properties(native_wow64_${contract}_fex PROPERTIES TIMEOUT 15 LABELS "fex;wow64;engine"
		ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
endforeach()
# Also run aligned SSE spills through the nested callback entry.
add_test(NAME native_wow64_alignment_callback_fex COMMAND fex_wow64_process_test ${WOW64_ALIGN} cdecl)
set_tests_properties(native_wow64_alignment_callback_fex PROPERTIES TIMEOUT 15 LABELS "fex;wow64;engine"
	ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
add_executable(fex_wow64_crt_test ${CMAKE_CURRENT_LIST_DIR}/wow64_crt_test.cpp)
target_link_libraries(fex_wow64_crt_test PRIVATE raven_cpu)
target_link_options(fex_wow64_crt_test PRIVATE -Wl,-platform_version,macos,11.0,11.0)
add_dependencies(fex_wow64_crt_test fex_wow64_alignment_crt_fixtures raven-bootstrap)
foreach(engine native fex)
	add_test(NAME native_ntdll_cdecl_x32_${engine} COMMAND fex_wow64_crt_test $<TARGET_FILE:raven-bootstrap> ${WOW64_CRT} ${engine})
	set_tests_properties(native_ntdll_cdecl_x32_${engine} PROPERTIES TIMEOUT 30
		ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT};RAVEN_PREFIX=${CMAKE_BINARY_DIR}/m4-crt-${engine}-prefix")
endforeach()
set_tests_properties(native_ntdll_cdecl_x32_fex PROPERTIES LABELS "fex;wow64;process")
add_executable(fex_wow64_boundary_test ${CMAKE_CURRENT_LIST_DIR}/wow64_boundary_test.cpp
	${CMAKE_CURRENT_LIST_DIR}/modhost/abi.S ${CMAKE_CURRENT_LIST_DIR}/modhost/context.S)
target_compile_features(fex_wow64_boundary_test PRIVATE cxx_std_17)
target_compile_options(fex_wow64_boundary_test PRIVATE -arch arm64 -ffixed-x18 -ffp-contract=off -Wall -Wextra -Werror -mmacosx-version-min=11.0)
target_compile_options(fex_wow64_boundary_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-UNDEBUG> -Wno-unused-function)
target_link_options(fex_wow64_boundary_test PRIVATE -arch arm64 -Wl,-platform_version,macos,11.0,11.0)
foreach(module wow64 x64)
	set(dll xtajit64.dll)
	if(module STREQUAL "wow64")
		set(dll xtajit.dll)
	endif()
	add_test(NAME native_${module}_host_av_rejection_fex COMMAND fex_wow64_boundary_test ${RAVEN_FEX_MODULE_ROOT}/${dll} ${module})
	set_tests_properties(native_${module}_host_av_rejection_fex PROPERTIES TIMEOUT 15 LABELS "fex;wow64;fault")
endforeach()

foreach(laa 0 1)
	set(fixture ${CMAKE_BINARY_DIR}/native/fex-range32-${laa}.exe)
	set(laa_flag --disable-large-address-aware)
	if(laa)
		set(laa_flag --large-address-aware)
	endif()
	add_custom_command(OUTPUT ${fixture}
		COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe -DEXPECT_LAA=${laa}
			-Wl,--entry,_raven_entry,--image-base,0x90000000,--enable-reloc-section,--no-insert-timestamp,${laa_flag}
			-o ${fixture} ${CMAKE_CURRENT_LIST_DIR}/wow64_range.c -lkernel32
		DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_range.c VERBATIM)
	add_custom_target(fex_wow64_range_${laa} DEPENDS ${fixture})
	add_dependencies(fex_wow64_process_test fex_wow64_range_${laa})
	add_test(NAME native_wow64_range_process_${laa}_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-wow64.py
		--raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
	set_tests_properties(native_wow64_range_process_${laa}_fex PROPERTIES TIMEOUT 30 LABELS "fex;wow64;process;placement")
endforeach()
# Once registered, WoW64 tests require the provisioned module.
set_property(TEST native_nt_service_fex APPEND PROPERTY ENVIRONMENT "RAVEN_FEX_REQUIRED=1")
