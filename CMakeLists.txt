cmake_minimum_required(VERSION 3.25)

project(vapoursynth-resdet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# VapourSynth 80's VSVulkan4 API requires Vulkan 1.4 headers. When a Vulkan
# SDK is installed, prefer its headers, loader, and CMake packages over older
# distribution packages such as Ubuntu's system libvulkan-dev.
set(VULKAN_SDK_ROOT "")
if(DEFINED ENV{VULKAN_SDK})
    set(VULKAN_SDK_ROOT "$ENV{VULKAN_SDK}")
    list(PREPEND CMAKE_PREFIX_PATH
        "${VULKAN_SDK_ROOT}"
        "${VULKAN_SDK_ROOT}/lib/cmake"
        "${VULKAN_SDK_ROOT}/Lib/cmake"
    )
endif()

find_package(Vulkan REQUIRED)

# VapourSynth 80 ships its development headers with the Python package. Query
# the active interpreter first, while keeping the cache variable overrideable
# for system installations and non-Python CMake consumers.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND NOT VAPOURSYNTH_INCLUDE_DIR)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c
            "import vapoursynth; print(vapoursynth.get_include())"
        OUTPUT_VARIABLE VAPOURSYNTH_PYTHON_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

find_path(VAPOURSYNTH_INCLUDE_DIR
    NAMES VapourSynth4.h VSVulkan4.h
    HINTS
        "${VAPOURSYNTH_PYTHON_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/.venv/lib/python3.13/site-packages/vapoursynth/include"
        "/opt/homebrew/include/vapoursynth"
        "/usr/local/include/vapoursynth"
)
if(NOT VAPOURSYNTH_INCLUDE_DIR)
    message(FATAL_ERROR
        "VapourSynth 80 headers were not found. Set VAPOURSYNTH_INCLUDE_DIR."
    )
endif()

# Prefer the Homebrew package installed for this checkout.  Its config file
# pulls in SPIRV-Tools-opt, so /opt/homebrew is also included in the prefix
# search path for that transitive dependency.
if(APPLE)
    list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew")
    if(NOT glslang_DIR
       AND EXISTS "/opt/homebrew/opt/glslang/lib/cmake/glslang")
        set(glslang_DIR "/opt/homebrew/opt/glslang/lib/cmake/glslang"
            CACHE PATH "glslang CMake package directory")
    endif()
endif()
if(VULKAN_SDK_ROOT AND NOT glslang_DIR)
    foreach(GLSLANG_CMAKE_DIR
        "${VULKAN_SDK_ROOT}/lib/cmake/glslang"
        "${VULKAN_SDK_ROOT}/Lib/cmake/glslang"
    )
        if(EXISTS "${GLSLANG_CMAKE_DIR}")
            set(glslang_DIR "${GLSLANG_CMAKE_DIR}"
                CACHE PATH "glslang CMake package directory")
            break()
        endif()
    endforeach()
endif()

# Homebrew and Linux development packages export glslang's own CMake package.
# The Windows Vulkan SDK ships the same libraries and headers, but some SDK
# layouts only expose them through FindVulkan instead of glslangConfig.cmake.
find_package(glslang CONFIG QUIET)
if(TARGET glslang::SPIRV)
    set(VS_RESD_GLSLANG_TARGET glslang::SPIRV)
else()
    find_package(Vulkan REQUIRED COMPONENTS glslang)
    if(NOT TARGET Vulkan::glslang)
        message(FATAL_ERROR
            "Could not find a usable glslang CMake package or Vulkan::glslang target."
        )
    endif()
    set(VS_RESD_GLSLANG_TARGET Vulkan::glslang)

    # FindVulkan's Vulkan::glslang target covers the glslang libraries shipped
    # by the SDK, but some SDK releases build glslang with SPIR-V Tools support
    # without expressing those static-library dependencies in the target.  In
    # that case SpvTools.obj leaves both the SPIR-V C API and optimizer symbols
    # unresolved at link time (notably with the Windows SDK).
    find_library(VS_RESD_SPIRV_TOOLS_OPT_LIBRARY
        NAMES SPIRV-Tools-opt SPIRV-Tools-optd
        HINTS
            "${VULKAN_SDK_ROOT}/lib"
            "${VULKAN_SDK_ROOT}/Lib"
    )
    find_library(VS_RESD_SPIRV_TOOLS_LIBRARY
        NAMES SPIRV-Tools SPIRV-Toolsd
        HINTS
            "${VULKAN_SDK_ROOT}/lib"
            "${VULKAN_SDK_ROOT}/Lib"
    )
    if(NOT VS_RESD_SPIRV_TOOLS_OPT_LIBRARY OR NOT VS_RESD_SPIRV_TOOLS_LIBRARY)
        message(FATAL_ERROR
            "The Vulkan SDK glslang fallback requires SPIRV-Tools-opt and "
            "SPIRV-Tools libraries. Set VULKAN_SDK or provide a glslang CMake package."
        )
    endif()

    add_library(vs-resdet-spirv-tools-opt UNKNOWN IMPORTED)
    set_target_properties(vs-resdet-spirv-tools-opt PROPERTIES
        IMPORTED_LOCATION "${VS_RESD_SPIRV_TOOLS_OPT_LIBRARY}"
    )
    add_library(vs-resdet-spirv-tools UNKNOWN IMPORTED)
    set_target_properties(vs-resdet-spirv-tools PROPERTIES
        IMPORTED_LOCATION "${VS_RESD_SPIRV_TOOLS_LIBRARY}"
    )
    list(APPEND VS_RESD_GLSLANG_TARGET
        vs-resdet-spirv-tools-opt
        vs-resdet-spirv-tools
    )
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unpack_y.comp" UNPACK_Y_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/score_methods.comp" SCORE_METHODS_SHADER)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unpack_y.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/score_methods.comp"
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shader_sources.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/shader_sources.hpp"
    @ONLY
)

find_program(GLSLANG_VALIDATOR
    NAMES glslangValidator
    HINTS
        "${VULKAN_SDK_ROOT}/bin"
        "${VULKAN_SDK_ROOT}/Bin"
        "/opt/homebrew/bin"
        "/usr/local/bin"
)
if(GLSLANG_VALIDATOR)
    set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(UNPACK_SHADER_SPV "${SHADER_OUTPUT_DIR}/unpack_y.comp.spv")
    set(SCORE_SHADER_SPV "${SHADER_OUTPUT_DIR}/score_methods.comp.spv")
    add_custom_command(
        OUTPUT "${UNPACK_SHADER_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" --target-env vulkan1.3 -V
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unpack_y.comp"
            -o "${UNPACK_SHADER_SPV}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unpack_y.comp"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SCORE_SHADER_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" --target-env vulkan1.3 -V
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/score_methods.comp"
            -o "${SCORE_SHADER_SPV}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/score_methods.comp"
        VERBATIM
    )
    add_custom_target(vs-resdet-shaders ALL
        DEPENDS "${UNPACK_SHADER_SPV}" "${SCORE_SHADER_SPV}")
else()
    message(FATAL_ERROR "glslangValidator is required to validate the resdet shaders")
endif()

add_library(vs-resdet-vkfft INTERFACE)
target_compile_definitions(vs-resdet-vkfft INTERFACE VKFFT_BACKEND=0)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/VkFFT/vkFFT")
    target_include_directories(vs-resdet-vkfft INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/VkFFT/vkFFT"
    )
else()
    message(WARNING "VkFFT submodule is not initialized; only the Vulkan smoke test will be available.")
endif()

add_library(vs-resdet SHARED
    src/vs_resdet.cpp
    src/vkfft_dispatch.hpp
)
add_dependencies(vs-resdet vs-resdet-shaders)
target_include_directories(vs-resdet PRIVATE
    "${VAPOURSYNTH_INCLUDE_DIR}"
    "${Vulkan_INCLUDE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(vs-resdet PRIVATE
    vs-resdet-vkfft
    ${VS_RESD_GLSLANG_TARGET}
)
target_compile_definitions(vs-resdet PRIVATE
    VS_USE_API_43=1
    VK_ENABLE_BETA_EXTENSIONS=1
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(vs-resdet PRIVATE
        -Wno-deprecated-declarations
        -Wno-comment
    )
endif()
set_target_properties(vs-resdet PROPERTIES
    PREFIX ""
    OUTPUT_NAME "vsresdet"
)

install(TARGETS vs-resdet
    LIBRARY DESTINATION lib/vapoursynth
    RUNTIME DESTINATION lib/vapoursynth
    ARCHIVE DESTINATION lib/vapoursynth
)
