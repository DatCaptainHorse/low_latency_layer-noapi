cmake_minimum_required(VERSION 3.10)
project(low_latency_layer
    VERSION 0.02
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE CXX_SOURCES src/*.cc)

set(LIBRARY_NAME VkLayer_KORTHOS_LowLatency)
add_library(${LIBRARY_NAME} SHARED
    ${CXX_SOURCES}
)

find_package(Vulkan REQUIRED)
find_package(VulkanUtilityLibraries REQUIRED)
target_link_libraries(${LIBRARY_NAME}
    Vulkan::Vulkan
    Vulkan::SafeStruct
)

target_include_directories(${LIBRARY_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

include(GNUInstallDirs)
# Prefix an absolute directory to our layer's json so the loader knows where the shared library is.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/low_latency_layer.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/low_latency_layer.json"
    @ONLY
)
install(TARGETS ${LIBRARY_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/low_latency_layer.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d/"
)

target_compile_options(${LIBRARY_NAME} PRIVATE
    # Inherited from Mesa.
    -g
    -O2
    -D_GLIBCXX_ASSERTIONS
    -D__STDC_CONSTANT_MACROS
    -D__STDC_FORMAT_MACROS
    -D__STDC_LIMIT_MACROS

    -Wall
    -Wextra
    -Wno-sign-conversion
    -Wno-missing-field-initializers

    -Wundef
    -Wconversion
    -Wdouble-promotion
)

# A tiny headless presenting application used to validate the layer's own
# Vulkan usage under VK_LAYER_KHRONOS_validation. Not installed.
option(LOW_LATENCY_LAYER_BUILD_TESTS "Build the validation harness" OFF)
if(LOW_LATENCY_LAYER_BUILD_TESTS)
    add_executable(paced_present test/paced_present.cc)
    target_link_libraries(paced_present Vulkan::Vulkan)

    # Optional: present to a real window, which is the only way to exercise
    # vsync backpressure.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(XCB QUIET xcb)
        if(XCB_FOUND)
            target_compile_definitions(paced_present PRIVATE
                PACED_PRESENT_HAVE_XCB)
            target_include_directories(paced_present PRIVATE
                ${XCB_INCLUDE_DIRS})
            target_link_libraries(paced_present ${XCB_LIBRARIES})
        endif()
    endif()

    add_executable(vblank_model_test
        test/vblank_model_test.cc
        src/vblank_model.cc
    )
    target_include_directories(vblank_model_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(vblank_model_test Vulkan::Vulkan)

    enable_testing()
    add_test(NAME vblank_model COMMAND vblank_model_test)
endif()
