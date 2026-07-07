cmake_minimum_required(VERSION 3.22 FATAL_ERROR)
get_filename_component(PROJECT_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)
project(${PROJECT_NAME} LANGUAGES C CXX)

# Prior adding the package one can disable certain libraries
# e.g.
option(NVPRO2_ENABLE_nvgl "" OFF)
option(NVPRO2_ENABLE_nvgpu_monitor "" OFF)

# Enable Slang integration within the nvpro_core2 framework
set(USE_SLANG ON CACHE BOOL "" FORCE)

# Call FindNvproCore2 to download nvpro_core2 or find it if it is already
# downloaded, or is located as a subdirectory or up to two levels up.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(NvproCore2 REQUIRED)
add_subdirectory(common)

#####################################################################################
# Source files for this project
#

set(SOURCE_FILES
  main.cpp
)
set(COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common")
set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

source_group("Source Files" FILES ${SOURCE_FILES})

#####################################################################################
# Executable
#

add_executable(${PROJECT_NAME} ${SOURCE_FILES})
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})

#------------------------------------------------------------------------------------------------------------------------------
# Compile shaders
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/_autogen")
file(GLOB SHADER_SLANG_FILES "shaders/*.slang")

file(GLOB SHADER_H_FILES "shaders/*.h" "shaders/*.h.slang")
list(FILTER SHADER_SLANG_FILES EXCLUDE REGEX ".*\\.h\\.slang$")

list(APPEND SHADER_SLANG_FILES
    ${NVSHADERS_DIR}/nvshaders/sky_simple.slang
    ${NVSHADERS_DIR}/nvshaders/tonemapper.slang
)

list(APPEND SHADER_SLANG_FILES ${COMMON_DIR}/shaders/foundation.slang)

# Build shader include flags
set(SHADER_INCLUDE_FLAGS "-I${NVSHADERS_DIR}" "-I${ROOT_DIR}")
foreach(include_dir ${RT_TUTORIAL_SHADER_INCLUDE_DIRS})
    list(APPEND SHADER_INCLUDE_FLAGS "-I${include_dir}")
endforeach()

set(ALL_CAPABILITIES spvGroupNonUniformBallot spvGroupNonUniformArithmetic spvGroupNonUniform)
list(APPEND ALL_CAPABILITIES ${RT_TUTORIAL_CAPABILITIES})

compile_slang(
    "${SHADER_SLANG_FILES}"
    "${SHADER_OUTPUT_DIR}"
    HEADERS_VAR GENERATED_SHADER_SLANG_HEADERS
    OPTIMIZATION_LEVEL 0
    DEBUG_LEVEL 2
    CAPABILITIES ${ALL_CAPABILITIES}
    EXTRA_FLAGS ${SHADER_INCLUDE_FLAGS}
)

# Add shader files to the project
source_group("Shaders" FILES ${SHADER_SLANG_FILES} ${SHADER_H_FILES})
source_group("Shaders/Compiled" FILES ${GENERATED_SHADER_SLANG_HEADERS} )

# Add the output shader headers (target) directly to the executable
# This allow to compile the shaders when the executable is built
target_sources(${PROJECT_NAME} PRIVATE ${SHADER_SLANG_FILES} ${GENERATED_SHADER_SLANG_HEADERS} ${SHADER_H_FILES})


target_link_libraries(${PROJECT_NAME} PRIVATE
  vk_raytracing_tutorial_common
  nvpro2::nvapp
  nvpro2::nvslang
  tinygltf
)

add_project_definitions(${PROJECT_NAME})

# This sample doesn't need addtional files, but one might need to
# copy required dlls, additional commands etc. through this command
copy_to_runtime_and_install(${PROJECT_NAME} AUTO)

install(DIRECTORY "${NVSHADERS_DIR}/nvshaders" DESTINATION ${CMAKE_INSTALL_PREFIX})
install(FILES ${Slang_GLSLANG} DESTINATION ${CMAKE_INSTALL_PREFIX})


