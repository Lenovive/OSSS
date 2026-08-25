cmake_minimum_required(VERSION 3.25)

project(OSSS VERSION 0.2.0 LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

if(UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(X11_FOUND)
        find_library(OSSS_XEXT_LIBRARY NAMES Xext)
        find_library(OSSS_XRANDR_LIBRARY NAMES Xrandr)
        find_path(OSSS_XRANDR_INCLUDE_DIR X11/extensions/Xrandr.h)
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

if(WIN32)
    set(OSSS_WINDOWS_MANIFEST src/osss.manifest)
endif()

# Every OSSS target links osss_common_flags. Put warning level, language
# conformance, and the Windows definitions HERE rather than repeating them per
# target: a new target that forgets a line would otherwise silently drop /W4 or
# build without UNICODE, and neither failure is visible until much later.
add_library(osss_common_flags INTERFACE)
if(WIN32)
    target_compile_definitions(osss_common_flags INTERFACE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
if(MSVC)
    target_compile_options(osss_common_flags INTERFACE /W4 /permissive- /EHsc)
else()
    target_compile_options(osss_common_flags INTERFACE -Wall -Wextra)
endif()

# Assertion helpers shared by every test executable (tests/test_harness.h).
add_library(osss_test_support INTERFACE)
target_include_directories(osss_test_support INTERFACE tests)
target_link_libraries(osss_test_support INTERFACE osss_common_flags)

# osss_core holds the modules with no Direct3D and no Windows Graphics Capture
# dependency: logic that a GPU-less machine can build, link, and test. Anything
# that needs a D3D device belongs in an executable's own source list or in a new
# library -- do not add it here just because more than one target uses it.
add_library(osss_core
    src/adaptive_scheduler.cpp
    src/app_profile.cpp
    src/launcher_theme.cpp
    src/platform/app_paths.cpp
    src/platform/unicode.cpp
    src/ui_mask.cpp
    src/window_catalog.cpp
)

target_include_directories(osss_core PUBLIC src)
if(WIN32)
    target_sources(osss_core PRIVATE src/window_catalog_win32.cpp)
    # window_catalog_win32.cpp calls DwmGetWindowAttribute and app_paths.cpp
    # calls SHGetKnownFolderPath, so the library declares those itself rather
    # than leaving every consumer to discover them at link time.
    target_link_libraries(osss_core PUBLIC osss_common_flags dwmapi shell32)
elseif(APPLE)
    target_sources(osss_core PRIVATE src/window_catalog_macos.mm)
    target_compile_options(osss_core PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(osss_core PUBLIC osss_common_flags
        "-framework Cocoa"
        "-framework CoreGraphics")
elseif(X11_FOUND)
    target_sources(osss_core PRIVATE src/window_catalog_x11.cpp)
    target_include_directories(osss_core PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(osss_core PUBLIC osss_common_flags ${X11_LIBRARIES})
    if(OSSS_XRANDR_LIBRARY AND OSSS_XRANDR_INCLUDE_DIR)
        target_compile_definitions(osss_core PRIVATE OSSS_HAVE_XRANDR=1)
        target_include_directories(osss_core PRIVATE ${OSSS_XRANDR_INCLUDE_DIR})
        target_link_libraries(osss_core PUBLIC ${OSSS_XRANDR_LIBRARY})
    endif()
else()
    target_sources(osss_core PRIVATE src/window_catalog_stub.cpp)
    target_link_libraries(osss_core PUBLIC osss_common_flags)
endif()

# --- Windows high-fidelity targets --------------------------------------------
# The original capture, GPU, overlay, and native settings paths remain the
# Windows/D3D11 implementation. Non-Windows builds use the portable runtime
# below; the common scheduler and core policy are shared by both paths.

if(WIN32)

add_executable(osss
    ${OSSS_WINDOWS_MANIFEST}
    src/main.cpp
    src/capture_smoke.cpp
    src/capture_session.cpp
    src/gpu_timing.cpp
    src/motion_interpolator.cpp
    src/renderer.cpp
    src/shader_cache.cpp
    src/upscaler.cpp
    src/stats_overlay.cpp
)

target_link_libraries(osss PRIVATE
    osss_core
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    avrt
    ole32
    shell32
    windowsapp
)

add_executable(osss_gui WIN32
    ${OSSS_WINDOWS_MANIFEST}
    src/gui_main.cpp
)

target_link_libraries(osss_gui PRIVATE
    osss_core
    comctl32
    dwmapi
    shell32
    uxtheme
)

endif() # WIN32

# The portable runtime keeps the scheduler and core policy shared with the
# Windows path while replacing D3D/WGC with a native desktop capture/present
# boundary and a dependency-free software interpolator. X11 is used on Linux
# when its development headers are available; macOS uses CoreGraphics/Cocoa.
if(NOT WIN32)
    add_library(osss_portable_runtime
        src/platform/desktop_backend.cpp
        src/platform/software_interpolator.cpp
    )
    target_include_directories(osss_portable_runtime PUBLIC src)
    target_link_libraries(osss_portable_runtime PUBLIC osss_common_flags)

    if(APPLE)
        target_sources(osss_portable_runtime PRIVATE src/platform/desktop_backend_macos.mm)
        target_link_libraries(osss_portable_runtime PUBLIC
            "-framework Cocoa"
            "-framework CoreGraphics")
        target_compile_options(osss_portable_runtime PRIVATE
            $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    elseif(X11_FOUND AND OSSS_XEXT_LIBRARY)
        target_sources(osss_portable_runtime PRIVATE src/platform/desktop_backend_x11.cpp)
        target_include_directories(osss_portable_runtime PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(osss_portable_runtime PUBLIC ${X11_LIBRARIES})
        if(TARGET X11::Xext)
            target_link_libraries(osss_portable_runtime PUBLIC X11::Xext)
        else()
            target_link_libraries(osss_portable_runtime PUBLIC ${OSSS_XEXT_LIBRARY})
        endif()
    else()
        target_sources(osss_portable_runtime PRIVATE src/platform/desktop_backend_stub.cpp)
    endif()

    add_executable(osss src/portable_main.cpp)
    target_link_libraries(osss PRIVATE osss_portable_runtime osss_core)

    add_executable(osss_gui src/portable_gui_main.cpp)
    target_link_libraries(osss_gui PRIVATE osss_core)
endif()

# The image side of the test harnesses: the analytic pattern and the two
# encoders that write it out. png_writer belongs here rather than in osss_core
# because every consumer is a dump path that already links this library, and
# pulling osss_core into the GPU benches would drag window_catalog and its
# Windows dependencies along with it.
add_library(osss_test_pattern
    src/frame_sequence.cpp
    src/png_writer.cpp
    src/test_pattern.cpp
)

target_include_directories(osss_test_pattern PUBLIC src)
target_link_libraries(osss_test_pattern PUBLIC osss_common_flags)

if(WIN32)

add_executable(osss_test_animation
    ${OSSS_WINDOWS_MANIFEST}
    src/test_animation_main.cpp
    src/test_animation_backends.cpp
)

target_link_libraries(osss_test_animation PRIVATE
    osss_test_pattern
    d3d9
    d3d10
    d3d11
    d3d12
    dwmapi
    dxgi
    shell32
)

add_dependencies(osss_gui osss_test_animation)

endif() # WIN32

enable_testing()

# --- CTest suite -------------------------------------------------------------
# Every add_test below runs unattended. Keep the table in CONTRIBUTING.md
# ("Which checks are safe to run unattended") in sync when adding one.

if(NOT WIN32)
    add_executable(osss_portable_backend_tests
        tests/portable_backend_tests.cpp
    )
    target_link_libraries(osss_portable_backend_tests PRIVATE
        osss_portable_runtime
        osss_core
        osss_test_support)
    add_test(NAME osss_portable_backend_tests COMMAND osss_portable_backend_tests)
endif()

add_executable(osss_adaptive_scheduler_tests
    tests/adaptive_scheduler_tests.cpp
)

target_link_libraries(osss_adaptive_scheduler_tests PRIVATE osss_core osss_test_support)

add_test(NAME osss_adaptive_scheduler_tests COMMAND osss_adaptive_scheduler_tests)

add_executable(osss_test_pattern_tests
    tests/test_pattern_tests.cpp
)

target_link_libraries(osss_test_pattern_tests PRIVATE osss_test_pattern osss_test_support)

add_test(NAME osss_test_pattern_tests COMMAND osss_test_pattern_tests)

# Round-trips the dependency-free PNG encoder through a minimal inflater and
# PNG parser carried in the test itself. There is no library behind the encoder,
# so a decode is the only evidence that what it writes is a real PNG.
add_executable(osss_png_writer_tests
    tests/png_writer_tests.cpp
)

target_link_libraries(osss_png_writer_tests PRIVATE osss_test_pattern osss_test_support)

add_test(NAME osss_png_writer_tests COMMAND osss_png_writer_tests)

# The three GPU benches below need a D3D11 device, so they only exist on
# Windows. Fails on GPU-less machines and under some sandboxes; that is an
# environment failure, not a source regression.
if(WIN32)

add_executable(osss_motion_tests
    tests/motion_interpolator_tests.cpp
    src/gpu_timing.cpp
    src/motion_interpolator.cpp
    src/shader_cache.cpp
    src/upscaler.cpp
    src/ui_mask.cpp
)

target_include_directories(osss_motion_tests PRIVATE src)
target_link_libraries(osss_motion_tests PRIVATE
    osss_test_support
    d3d11
    d3dcompiler
    dxgi
    ole32
    shell32
    windowsapp
)

add_test(NAME osss_motion_tests COMMAND osss_motion_tests)

# Reference-image quality bench for the upscaler: a downscale/upscale round trip
# scored against the original, with bilinear as the baseline it has to beat.
add_executable(osss_upscaler_tests
    tests/upscaler_tests.cpp
    src/gpu_timing.cpp
    src/shader_cache.cpp
    src/upscaler.cpp
)

target_include_directories(osss_upscaler_tests PRIVATE src)
target_link_libraries(osss_upscaler_tests PRIVATE
    osss_test_pattern
    osss_test_support
    d3d11
    d3dcompiler
    dxgi
    ole32
    shell32
    windowsapp
)

add_test(NAME osss_upscaler_tests COMMAND osss_upscaler_tests)

# Reference-image quality bench: scores MotionInterpolator against the analytic
# test pattern. Needs a D3D11 device; prefers hardware and falls back to WARP.
add_executable(osss_interpolation_quality_tests
    tests/interpolation_quality_tests.cpp
    src/gpu_timing.cpp
    src/motion_interpolator.cpp
    src/shader_cache.cpp
    src/upscaler.cpp
    src/ui_mask.cpp
)

target_include_directories(osss_interpolation_quality_tests PRIVATE src)
target_link_libraries(osss_interpolation_quality_tests PRIVATE
    osss_test_pattern
    osss_test_support
    d3d11
    d3dcompiler
    dxgi
    ole32
    shell32
    windowsapp
)

add_test(NAME osss_interpolation_quality_tests COMMAND osss_interpolation_quality_tests)

endif() # WIN32

add_executable(osss_output_mode_tests
    tests/output_mode_tests.cpp
)

target_link_libraries(osss_output_mode_tests PRIVATE osss_core osss_test_support)

add_test(NAME osss_output_mode_tests COMMAND osss_output_mode_tests)

add_executable(osss_pacing_mode_tests
    tests/pacing_mode_tests.cpp
)

target_link_libraries(osss_pacing_mode_tests PRIVATE osss_core osss_test_support)

add_test(NAME osss_pacing_mode_tests COMMAND osss_pacing_mode_tests)

add_executable(osss_app_profile_tests
    tests/app_profile_tests.cpp
)

target_link_libraries(osss_app_profile_tests PRIVATE osss_core osss_test_support)

add_test(NAME osss_app_profile_tests COMMAND osss_app_profile_tests)

add_executable(osss_debug_view_tests
    tests/debug_view_tests.cpp
)

target_link_libraries(osss_debug_view_tests PRIVATE osss_core osss_test_support)

add_test(NAME osss_debug_view_tests COMMAND osss_debug_view_tests)

add_executable(osss_flow_scale_tests
    tests/flow_scale_tests.cpp
)

target_link_libraries(osss_flow_scale_tests PRIVATE osss_core osss_test_support)

add_test(NAME osss_flow_scale_tests COMMAND osss_flow_scale_tests)

add_executable(osss_launcher_layout_tests
    tests/launcher_layout_tests.cpp
)

target_link_libraries(osss_launcher_layout_tests PRIVATE osss_core osss_test_support)

add_test(NAME osss_launcher_layout_tests COMMAND osss_launcher_layout_tests)

# Target-window selection: that an executable-name match outranks a title
# match, so a shell whose title is the command line cannot make an unambiguous
# --title request ambiguous. Pure ranking only; enumeration needs a desktop.
add_executable(osss_window_catalog_tests
    tests/window_catalog_tests.cpp
)

target_link_libraries(osss_window_catalog_tests PRIVATE osss_core osss_test_support)

add_test(NAME osss_window_catalog_tests COMMAND osss_window_catalog_tests)

add_executable(osss_ui_mask_tests
    tests/ui_mask_tests.cpp
)

target_link_libraries(osss_ui_mask_tests PRIVATE osss_core osss_test_support)

add_test(NAME osss_ui_mask_tests COMMAND osss_ui_mask_tests)

# Covers the header-only telemetry maths in stats_overlay.h. The overlay's own
# .cpp needs a window, so it is deliberately not linked here.
add_executable(osss_stats_overlay_tests
    tests/stats_overlay_tests.cpp
)

target_link_libraries(osss_stats_overlay_tests PRIVATE osss_core osss_test_support)

add_test(NAME osss_stats_overlay_tests COMMAND osss_stats_overlay_tests)

if(WIN32)

# Creates a window, so it needs an interactive desktop session.
add_test(NAME osss_gui_tests COMMAND osss_gui --self-test)

# This smoke test intentionally moves and clicks the system pointer, so build it
# for explicit compatibility checks without running it during ordinary CTest.
add_executable(osss_input_passthrough_smoke
    ${OSSS_WINDOWS_MANIFEST}
    tests/input_passthrough_smoke.cpp
    src/gpu_timing.cpp
    src/motion_interpolator.cpp
    src/renderer.cpp
    src/shader_cache.cpp
    src/upscaler.cpp
)

target_include_directories(osss_input_passthrough_smoke PRIVATE src)
target_link_libraries(osss_input_passthrough_smoke PRIVATE
    osss_core
    osss_test_support
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    ole32
    shell32
    windowsapp
)

endif() # WIN32
