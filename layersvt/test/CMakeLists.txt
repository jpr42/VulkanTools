# ~~~
# Copyright (c) 2023-2025 Valve Corporation
# Copyright (c) 2023-2025 LunarG, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ~~~
if (ANDROID)
    return()
endif()

function(LayerTest NAME)
	set(TEST_FILENAME ./test_${NAME}.cpp)
    set(TEST_NAME test_${NAME}_layer)

    file(GLOB TEST_JSON_FILES ${CMAKE_SOURCE_DIR}/profiles/test/data/)

    add_executable(${TEST_NAME}
                   ${TEST_FILENAME}
                   layer_test_helper.h
                   layer_test_helper.cpp
                   layer_test_main.cpp
                   layer_test_framework.cpp
                   layer_test_framework.h)
    add_dependencies(${TEST_NAME} VkLayer_${NAME})
    target_link_libraries(${TEST_NAME} Vulkan::Headers Vulkan::Loader GTest::gtest GTest::gtest_main Vulkan::LayerSettings)
    target_compile_definitions(${TEST_NAME} PUBLIC LAYER_BINARY_PATH="$<TARGET_FILE_DIR:VkLayer_${NAME}>")
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

    set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT
        "VK_LAYER_PATH=$<TARGET_FILE_DIR:VkLayer_${NAME}>"
    )

    set_target_properties(${TEST_NAME} PROPERTIES FOLDER "layers/${NAME}/Test")

    if(WIN32 AND (QT_TARGET_TYPE STREQUAL STATIC_LIBRARY))
      set_property(TARGET ${TEST_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()

# The monitor layer tests run against the mock driver from Vulkan-Tools when update_deps.py built it,
# so they don't need a GPU. Not on GitHub's Windows runners though: they run elevated, and the loader
# ignores VK_LAYER_PATH and VK_DRIVER_FILES in elevated processes.
if (TARGET VkLayer_monitor AND UPDATE_DEPS_DIR AND NOT (WIN32 AND RUN_ON_GITHUB))
    find_file(MOCK_ICD_JSON VkICD_mock_icd.json
              PATHS ${UPDATE_DEPS_DIR}/Vulkan-Tools/build/icd
              PATH_SUFFIXES Debug Release RelWithDebInfo MinSizeRel
              NO_DEFAULT_PATH NO_CACHE)
endif()

# These tests need a Vulkan driver, so they only run on LunarG's internal C.I.
if (NOT RUN_ON_GITHUB)
    set(LAYER_TEST_FILES api_dump screenshot)
    if (NOT MOCK_ICD_JSON)
        list(APPEND LAYER_TEST_FILES monitor)
    endif()

    foreach(test_item ${LAYER_TEST_FILES})
        # If the target doesn't exist continue.
        if (NOT TARGET "VkLayer_${test_item}")
            continue()
        endif()

        LayerTest(${test_item})
    endforeach()
endif()

if (MOCK_ICD_JSON)
    LayerTest(monitor)
    # The mock ICD manifest has a relative library path, which the Windows loader can't join to a path with forward slashes
    file(TO_NATIVE_PATH "${MOCK_ICD_JSON}" MOCK_ICD_JSON_NATIVE)
    set_property(TEST test_monitor_layer APPEND PROPERTY ENVIRONMENT "VK_DRIVER_FILES=${MOCK_ICD_JSON_NATIVE}")
elseif (TARGET VkLayer_monitor)
    message(STATUS "Not using the mock ICD, test_monitor_layer needs a Vulkan driver")
endif()

# Monitor overlay logic doesn't need a Vulkan driver
if (TARGET VkLayer_monitor)
    add_executable(test_monitor_overlay
                   test_monitor_overlay.cpp
                   ../monitor_overlay.cpp
                   ../monitor_overlay.h
                   layer_test_main.cpp)
    target_include_directories(test_monitor_overlay PRIVATE ..)
    target_compile_definitions(test_monitor_overlay PRIVATE MONITOR_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../shaders")
    target_link_libraries(test_monitor_overlay Vulkan::Headers GTest::gtest)
    add_test(NAME test_monitor_overlay COMMAND test_monitor_overlay)
    set_target_properties(test_monitor_overlay PROPERTIES FOLDER "layers/monitor/Test")

    # Swapchain and present plumbing against a fake device
    add_executable(test_monitor_present
                   test_monitor_present.cpp
                   ../monitor_present.cpp
                   ../monitor_present.h
                   ../monitor_renderer.cpp
                   ../monitor_renderer.h
                   ../monitor_overlay.cpp
                   ../monitor_overlay.h
                   layer_test_main.cpp)
    target_include_directories(test_monitor_present PRIVATE ..)
    target_link_libraries(test_monitor_present Vulkan::Headers Vulkan::UtilityHeaders GTest::gtest)
    add_test(NAME test_monitor_present COMMAND test_monitor_present)
    set_target_properties(test_monitor_present PROPERTIES FOLDER "layers/monitor/Test")

    # Draws the overlay with a real Vulkan driver, and skips without one
    add_executable(test_monitor_renderer
                   test_monitor_renderer.cpp
                   ../monitor_renderer.cpp
                   ../monitor_renderer.h
                   ../monitor_overlay.cpp
                   ../monitor_overlay.h
                   layer_test_main.cpp)
    target_include_directories(test_monitor_renderer PRIVATE ..)
    target_link_libraries(test_monitor_renderer Vulkan::Headers Vulkan::UtilityHeaders Vulkan::Loader GTest::gtest)
    add_test(NAME test_monitor_renderer COMMAND test_monitor_renderer)
    set_target_properties(test_monitor_renderer PROPERTIES FOLDER "layers/monitor/Test")

    if(WIN32 AND (QT_TARGET_TYPE STREQUAL STATIC_LIBRARY))
      set_property(TARGET test_monitor_overlay test_monitor_present test_monitor_renderer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()
