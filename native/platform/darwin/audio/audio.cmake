target_sources(raven_cpu PRIVATE ${RAVEN_DARWIN_DIR}/audio/api.cpp ${RAVEN_DARWIN_DIR}/audio/playsound.cpp)
add_library(raven_darwin_audio STATIC ${RAVEN_DARWIN_DIR}/audio/output.cpp ${RAVEN_DARWIN_DIR}/audio/mixer.cpp ${RAVEN_DARWIN_DIR}/audio/input.mm)
target_link_libraries(raven_darwin_audio PUBLIC "-framework AudioToolbox" "-framework CoreAudio" "-framework AVFoundation")
target_compile_options(raven_darwin_audio PRIVATE -Wall -Wextra)
target_link_libraries(raven_cpu PUBLIC raven_darwin_audio)
target_link_options(raven PRIVATE "-Wl,-sectcreate,__TEXT,__info_plist,${RAVEN_DARWIN_DIR}/audio/microphone.plist")
set_property(TARGET raven APPEND PROPERTY LINK_DEPENDS ${RAVEN_DARWIN_DIR}/audio/microphone.plist)
add_executable(darwin_audio_test ${RAVEN_DARWIN_DIR}/audio/output_test.cpp)
target_link_libraries(darwin_audio_test PRIVATE raven_darwin_audio)
add_test(NAME native_darwin_audio_offline COMMAND darwin_audio_test)
set_tests_properties(native_darwin_audio_offline PROPERTIES TIMEOUT 30)
# No microphone and no audible samples. Skip on machines without an output device.
add_test(NAME native_darwin_audio_device COMMAND darwin_audio_test --device)
set_tests_properties(native_darwin_audio_device PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
add_executable(darwin_capture_probe ${RAVEN_DARWIN_DIR}/audio/capture_probe.mm)
target_link_libraries(darwin_capture_probe PRIVATE raven_darwin_audio)
add_test(NAME native_darwin_capture_authorized COMMAND darwin_capture_probe)
set_tests_properties(native_darwin_capture_authorized PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
if(RAVEN_MINGW64)
    foreach(width IN ITEMS 64 32)
        if(width EQUAL 64)
            set(audio_cc ${RAVEN_MINGW64})
        else()
            set(audio_cc ${RAVEN_MINGW32})
        endif()
        if(audio_cc)
            set(audio_fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-wave${width}.exe)
            add_custom_command(OUTPUT ${audio_fixture}
                COMMAND ${audio_cc} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry
                    -o ${audio_fixture} ${RAVEN_DARWIN_DIR}/audio/wave_client.c -lwinmm -lkernel32
                DEPENDS ${RAVEN_DARWIN_DIR}/audio/wave_client.c VERBATIM)
            add_custom_target(darwin_wave${width}_fixture ALL DEPENDS ${audio_fixture})
            add_test(NAME native_darwin_wave_x${width} COMMAND raven --run-process --execution=native --timeout-ms=10000 ${audio_fixture})
            set_tests_properties(native_darwin_wave_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                SKIP_REGULAR_EXPRESSION "Native entry returned 77 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-wave-prefix${width};RAVEN_AUDIO_TRACE=1")
        endif()
    endforeach()
endif()
if(RAVEN_MINGW64 AND RAVEN_MINGW32)
    foreach(width IN ITEMS 64 32)
        if(width EQUAL 64)
            set(audio_cc ${RAVEN_MINGW64})
        else()
            set(audio_cc ${RAVEN_MINGW32})
        endif()
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-dsound${width}.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${audio_cc} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry
                -o ${fixture} ${RAVEN_DARWIN_DIR}/audio/dsound_client.c -lkernel32 -luser32 -ldxguid
            DEPENDS ${RAVEN_DARWIN_DIR}/audio/dsound_client.c VERBATIM)
        add_custom_target(darwin_dsound${width}_fixture ALL DEPENDS ${fixture})
        add_test(NAME native_darwin_dsound_x${width} COMMAND raven --run-process --execution=native --timeout-ms=10000 ${fixture})
        set_tests_properties(native_darwin_dsound_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-dsound-prefix${width};RAVEN_AUDIO_TRACE=1")
    endforeach()
endif()

foreach(width IN ITEMS 64 32)
    if(width EQUAL 64)
        set(audio_cc ${RAVEN_MINGW64})
    else()
        set(audio_cc ${RAVEN_MINGW32})
    endif()
    if(audio_cc)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-wasapi${width}.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${audio_cc} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry
                -o ${fixture} ${RAVEN_DARWIN_DIR}/audio/wasapi_client.c -lole32 -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/audio/wasapi_client.c VERBATIM)
        add_custom_target(darwin_wasapi${width}_fixture ALL DEPENDS ${fixture})
        add_test(NAME native_darwin_wasapi_x${width} COMMAND raven --run-process --execution=native --timeout-ms=10000 ${fixture})
        set_tests_properties(native_darwin_wasapi_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-wasapi-prefix${width};RAVEN_AUDIO_TRACE=1")
    endif()
endforeach()

foreach(width IN ITEMS 64 32)
    if(width EQUAL 64)
        set(audio_cc ${RAVEN_MINGW64})
    else()
        set(audio_cc ${RAVEN_MINGW32})
    endif()
    if(audio_cc)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-wavein${width}.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${audio_cc} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry
                -o ${fixture} ${RAVEN_DARWIN_DIR}/audio/wavein_client.c -lwinmm -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/audio/wavein_client.c VERBATIM)
        add_custom_target(darwin_wavein${width}_fixture ALL DEPENDS ${fixture})
        add_test(NAME native_darwin_wavein_x${width} COMMAND raven --run-process --execution=native --timeout-ms=10000 ${fixture})
        set_tests_properties(native_darwin_wavein_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-wavein-prefix${width}")
    endif()
endforeach()

foreach(width IN ITEMS 64 32)
    if(width EQUAL 64)
        set(audio_cc ${RAVEN_MINGW64})
    else()
        set(audio_cc ${RAVEN_MINGW32})
    endif()
    if(audio_cc)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-xaudio7-${width}.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${audio_cc} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry
                -o ${fixture} ${RAVEN_DARWIN_DIR}/audio/xaudio7_client.c -lole32 -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/audio/xaudio7_client.c VERBATIM)
        add_custom_target(darwin_xaudio7_${width}_fixture ALL DEPENDS ${fixture})
        add_test(NAME native_darwin_xaudio7_x${width} COMMAND raven --run-process --execution=native --timeout-ms=10000 ${fixture})
        set_tests_properties(native_darwin_xaudio7_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-xaudio7-prefix${width};RAVEN_AUDIO_TRACE=1")
    endif()
endforeach()

foreach(width IN ITEMS 64 32)
    if(width EQUAL 64)
        set(audio_cc ${RAVEN_MINGW64})
    else()
        set(audio_cc ${RAVEN_MINGW32})
    endif()
    if(audio_cc)
        set(fixture_wav ${CMAKE_CURRENT_BINARY_DIR}/darwin-playsound-silent.wav)
        add_custom_command(OUTPUT ${fixture_wav}
            COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/audio/make_silent_wave.py ${fixture_wav}
            DEPENDS ${RAVEN_DARWIN_DIR}/audio/make_silent_wave.py VERBATIM)
        list(GET audio_cc 0 audio_compiler)
        string(REGEX REPLACE "gcc$" "windres" audio_windres "${audio_compiler}")
        set(fixture_resource ${CMAKE_CURRENT_BINARY_DIR}/darwin-playsound${width}-resource.o)
        add_custom_command(OUTPUT ${fixture_resource}
            COMMAND ${audio_windres} -O coff -I ${CMAKE_CURRENT_BINARY_DIR}
                -i ${RAVEN_DARWIN_DIR}/audio/playsound.rc -o ${fixture_resource}
            DEPENDS ${RAVEN_DARWIN_DIR}/audio/playsound.rc ${fixture_wav} VERBATIM)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-playsound${width}.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${audio_cc} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry
                -o ${fixture} ${RAVEN_DARWIN_DIR}/audio/playsound_client.c ${fixture_resource} -lwinmm -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/audio/playsound_client.c ${fixture_resource} VERBATIM)
        add_custom_target(darwin_playsound${width}_fixture ALL DEPENDS ${fixture})
        add_test(NAME native_darwin_playsound_x${width} COMMAND raven --run-process --execution=native --timeout-ms=10000 ${fixture})
        set_tests_properties(native_darwin_playsound_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-playsound-prefix${width};RAVEN_AUDIO_TRACE=1")
    endif()
endforeach()

foreach(width IN ITEMS 64 32)
    if(width EQUAL 64)
        set(audio_cc ${RAVEN_MINGW64})
    else()
        set(audio_cc ${RAVEN_MINGW32})
    endif()
    if(audio_cc)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-xaudio${width}.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${audio_cc} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry
                -o ${fixture} ${RAVEN_DARWIN_DIR}/audio/xaudio_client.c -lxaudio2_9 -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/audio/xaudio_client.c VERBATIM)
        add_custom_target(darwin_xaudio${width}_fixture ALL DEPENDS ${fixture})
        add_test(NAME native_darwin_xaudio_x${width} COMMAND raven --run-process --execution=native --timeout-ms=10000 ${fixture})
        set_tests_properties(native_darwin_xaudio_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-xaudio-prefix${width};RAVEN_AUDIO_TRACE=1")
        # Callbacks execute while Sleep(250) owns a scheduler wait. This also regresses nested execution waits
        # clearing its deadline.
        add_test(NAME native_darwin_xaudio_parallel_x${width} COMMAND raven --run-process --execution=native --timeout-ms=10000 ${fixture})
        set_tests_properties(native_darwin_xaudio_parallel_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-xaudio-prefix${width};RAVEN_AUDIO_TRACE=1;RAVEN_PARALLEL=1")
        set(lifecycle_fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-xaudio-lifecycle${width}.exe)
        add_custom_command(OUTPUT ${lifecycle_fixture}
            COMMAND ${audio_cc} -O2 -nostdlib -fno-builtin -fno-stack-protector -DAUDIO_LIFECYCLE -Wl,--entry,raven_entry
                -o ${lifecycle_fixture} ${RAVEN_DARWIN_DIR}/audio/xaudio_client.c -lxaudio2_9 -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/audio/xaudio_client.c VERBATIM)
        add_custom_target(darwin_xaudio_lifecycle${width}_fixture ALL DEPENDS ${lifecycle_fixture})
        add_test(NAME native_darwin_xaudio_lifecycle_x${width} COMMAND raven --run-process --execution=native --timeout-ms=10000 ${lifecycle_fixture})
        set_tests_properties(native_darwin_xaudio_lifecycle_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-xaudio-prefix${width};RAVEN_AUDIO_TRACE=1;RAVEN_PARALLEL=1")
    endif()
endforeach()
