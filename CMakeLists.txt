cmake_minimum_required(VERSION 3.14)
project(DeckLinkDX11 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Get Git Commit Hash ---
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "unknown")
endif()
message(STATUS "Build commit: ${GIT_COMMIT_HASH}")

# --- Dependencies ---

# 1. DeckLink SDK API (Bumped to 1 as ImGui removed)

# 2. DeckLink SDK API
# Try to find DeckLink SDK. 
# Look in vendor, vender (typo), and standard paths
if(NOT DEFINED DECKLINK_SDK_PATH)
    if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/Blackmagic DeckLink SDK 15.3")
        set(DECKLINK_SDK_PATH "${CMAKE_SOURCE_DIR}/vendor/Blackmagic DeckLink SDK 15.3")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/vender/Blackmagic DeckLink SDK 15.3") # Handle typo
        set(DECKLINK_SDK_PATH "${CMAKE_SOURCE_DIR}/vender/Blackmagic DeckLink SDK 15.3")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/vendor/DeckLinkSDK")
        set(DECKLINK_SDK_PATH "${CMAKE_SOURCE_DIR}/vendor/DeckLinkSDK")
    endif()
endif()

if(EXISTS "${DECKLINK_SDK_PATH}/Win/include/DeckLinkAPI.idl")
    message(STATUS "Found DeckLink SDK (IDL) at ${DECKLINK_SDK_PATH}")
    include_directories(${DECKLINK_SDK_PATH}/Win/include)
    
    # We need to compile IDL files if using MSVC
    # Adding them to sources usually triggers MIDL in VS generator
    set(DECKLINK_SOURCES 
        "${DECKLINK_SDK_PATH}/Win/include/DeckLinkAPI.idl"
    )
    # The generated header will be accessible. 
    # Usually generated as DeckLinkAPI_h.h or DeckLinkAPI.h 
    # We might need to include CMAKE_CURRENT_BINARY_DIR to find it?
    # Actually VS puts it in intermediate dir. 
    # But let's check.
    
elseif(EXISTS "${DECKLINK_SDK_PATH}/include/DeckLinkAPI.h")
    # Older or custom structure
    message(STATUS "Found DeckLink SDK (header style) at ${DECKLINK_SDK_PATH}")
    include_directories(${DECKLINK_SDK_PATH}/include)
    set(DECKLINK_SOURCES 
        ${DECKLINK_SDK_PATH}/include/DeckLinkAPI_i.c
    )
else()
    message(WARNING "DeckLink SDK not found. Checked: ${DECKLINK_SDK_PATH}")
endif()


# 3. CEF
set(CEF_ROOT "${CMAKE_SOURCE_DIR}/vender/cef")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")

# Force CEF to use dynamic runtime (MD) to match our app
set(CEF_RUNTIME_LIBRARY_FLAG "/MD")

if(EXISTS "${CEF_ROOT}/CMakeLists.txt")
    message(STATUS "Found CEF at ${CEF_ROOT}")
    add_subdirectory("${CEF_ROOT}" "${CMAKE_BINARY_DIR}/cef_build")
else()
    message(FATAL_ERROR "CEF not found at ${CEF_ROOT}. Please run download script.")
endif()

# --- Sources ---
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h")

# Console App (Removed WIN32 flag)
add_executable(${PROJECT_NAME} 
    ${SOURCES}
    ${DECKLINK_SOURCES}
)

# Pass git commit hash as compile-time definition
target_compile_definitions(${PROJECT_NAME} PRIVATE
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
)

# --- Linking ---

# Link CEF
target_link_libraries(${PROJECT_NAME} PRIVATE libcef_dll_wrapper libcef)

# Add custom build step to copy CEF resources to the output directory
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CEF_ROOT}/Resources"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Copying CEF Resources..."
)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CEF_ROOT}/Release"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Copying CEF Binaries..."
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/src/render/shaders"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
    COMMENT "Copying Shaders..."
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/config.json"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Copying config.json..."
)

# Create installer using Inno Setup (if available)
find_program(INNO_SETUP_COMPILER ISCC.exe
    PATHS
    "C:/Program Files (x86)/Inno Setup 6"
    "C:/Program Files/Inno Setup 6"
    "C:/Program Files (x86)/Inno Setup 5"
    "C:/Program Files/Inno Setup 5"
    DOC "Inno Setup Compiler"
)

if(INNO_SETUP_COMPILER)
    message(STATUS "Found Inno Setup: ${INNO_SETUP_COMPILER}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${INNO_SETUP_COMPILER} "${CMAKE_SOURCE_DIR}/installer.iss"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Creating installer with Inno Setup..."
    )
else()
    message(WARNING "Inno Setup not found. Installer will not be created. Download from: https://jrsoftware.org/isdl.php")
endif()


# Include Directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/render
    ${CMAKE_SOURCE_DIR}/src/decklink
    ${CMAKE_SOURCE_DIR}/src/cef
    ${CEF_ROOT}
    ${CMAKE_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.dir/$<CONFIG>
)

# Link Directories
target_link_directories(${PROJECT_NAME} PRIVATE ${CEF_ROOT}/Release)

target_link_libraries(${PROJECT_NAME} PRIVATE 
    d3d11.lib 
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
    dbghelp.lib
    libcef_dll_wrapper
    "${CEF_ROOT}/Release/libcef.lib"
    gdi32.lib
    user32.lib
)

# Unicode support for Win32
add_definitions(-DUNICODE -D_UNICODE)
