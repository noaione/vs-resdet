cmake_minimum_required(VERSION 3.25)

project(vs-resdet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew")
set(glslang_DIR "/opt/homebrew/opt/glslang/lib/cmake/glslang"
    CACHE PATH "glslang CMake package directory")
find_package(glslang CONFIG REQUIRED)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/unpack_y.comp" UNPACK_Y_SHADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sign_scores.comp" SIGN_SCORES_SHADER)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shader_sources.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/shader_sources.hpp"
    @ONLY
)

find_program(GLSLANG_VALIDATOR
    NAMES glslangValidator
    HINTS "/opt/homebrew/bin" "/usr/local/bin"
)
if(GLSLANG_VALIDATOR)
    set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(UNPACK_SHADER_SPV "${SHADER_OUTPUT_DIR}/unpack_y.comp.spv")
    set(SIGN_SHADER_SPV "${SHADER_OUTPUT_DIR}/sign_scores.comp.spv")
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
        OUTPUT "${SIGN_SHADER_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" --target-env vulkan1.3 -V
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sign_scores.comp"
            -o "${SIGN_SHADER_SPV}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sign_scores.comp"
        VERBATIM
    )
    add_custom_target(vs-resdet-shaders ALL
        DEPENDS "${UNPACK_SHADER_SPV}" "${SIGN_SHADER_SPV}")
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
    glslang::SPIRV
)
target_compile_definitions(vs-resdet PRIVATE
    VS_USE_API_43=1
    VK_ENABLE_BETA_EXTENSIONS=1
)
target_compile_options(vs-resdet PRIVATE
    -Wno-deprecated-declarations
    -Wno-comment
)
set_target_properties(vs-resdet PROPERTIES
    PREFIX ""
    OUTPUT_NAME "vsresdet"
)

install(TARGETS vs-resdet
    LIBRARY DESTINATION lib/vapoursynth
    RUNTIME DESTINATION lib/vapoursynth
    ARCHIVE DESTINATION lib/vapoursynth
)
