cmake_minimum_required(VERSION 3.21)
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

# 1. DeckLink SDK API
if(NOT DEFINED DECKLINK_SDK_PATH)
    if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/Blackmagic DeckLink SDK 15.3")
        set(DECKLINK_SDK_PATH "${CMAKE_SOURCE_DIR}/vendor/Blackmagic DeckLink SDK 15.3")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/vender/Blackmagic DeckLink SDK 15.3") # Handle typo
        set(DECKLINK_SDK_PATH "${CMAKE_SOURCE_DIR}/vender/Blackmagic DeckLink SDK 15.3")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/vendor/DeckLinkSDK")
        set(DECKLINK_SDK_PATH "${CMAKE_SOURCE_DIR}/vendor/DeckLinkSDK")
    endif()
endif()

if(WIN32)
    if(EXISTS "${DECKLINK_SDK_PATH}/Win/include/DeckLinkAPI.idl")
        message(STATUS "Found DeckLink SDK (IDL) at ${DECKLINK_SDK_PATH}")
        include_directories(${DECKLINK_SDK_PATH}/Win/include)
        set(DECKLINK_SOURCES 
            "${DECKLINK_SDK_PATH}/Win/include/DeckLinkAPI.idl"
        )
    elseif(EXISTS "${DECKLINK_SDK_PATH}/include/DeckLinkAPI.h")
        message(STATUS "Found DeckLink SDK (header style) at ${DECKLINK_SDK_PATH}")
        include_directories(${DECKLINK_SDK_PATH}/include)
        set(DECKLINK_SOURCES 
            ${DECKLINK_SDK_PATH}/include/DeckLinkAPI_i.c
        )
    else()
        message(WARNING "DeckLink SDK not found. Checked: ${DECKLINK_SDK_PATH}")
    endif()
elseif(APPLE)
    if(EXISTS "${DECKLINK_SDK_PATH}/Mac/include/DeckLinkAPI.h")
        message(STATUS "Found DeckLink SDK (Mac headers) at ${DECKLINK_SDK_PATH}")
        include_directories(${DECKLINK_SDK_PATH}/Mac/include)
        set(DECKLINK_SOURCES 
            "${DECKLINK_SDK_PATH}/Mac/include/DeckLinkAPIDispatch.cpp"
        )
    else()
        message(WARNING "DeckLink SDK not found for Mac. Checked: ${DECKLINK_SDK_PATH}")
    endif()
endif()

# 2. CEF
set(CEF_ROOT "${CMAKE_SOURCE_DIR}/vender/cef")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")

if(WIN32)
    set(CEF_RUNTIME_LIBRARY_FLAG "/MD")
endif()

if(EXISTS "${CEF_ROOT}/CMakeLists.txt")
    message(STATUS "Found CEF at ${CEF_ROOT}")
    add_subdirectory("${CEF_ROOT}" "${CMAKE_BINARY_DIR}/cef_build")
else()
    message(FATAL_ERROR "CEF not found at ${CEF_ROOT}. Please run download script.")
endif()

if(APPLE)
  message(STATUS "Defining COPY_MAC_FRAMEWORK macro")
  macro(COPY_MAC_FRAMEWORK target binary_path app_path)
    message(STATUS "COPY_MAC_FRAMEWORK macro called for target: ${target}, binary_path: ${binary_path}, app_path: ${app_path}")
    set(FRAMEWORK_DIR "${app_path}/Contents/Frameworks/Chromium Embedded Framework.framework")
    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${binary_path}/Chromium Embedded Framework.framework"
              "${FRAMEWORK_DIR}/Versions/A"
      COMMAND cd "${FRAMEWORK_DIR}" && ln -sf
              "Versions/A/Chromium Embedded Framework"
              "Chromium Embedded Framework"
      COMMAND cd "${FRAMEWORK_DIR}" && ln -sf
              "Versions/A/Libraries"
              "Libraries"
      COMMAND cd "${FRAMEWORK_DIR}" && ln -sf
              "Versions/A/Resources"
              "Resources"
      COMMAND cd "${FRAMEWORK_DIR}/Versions" && ln -sf
              "A"
              "Current"
      VERBATIM
      )
  endmacro()
endif()

# --- Sources ---
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h" "src/*.rc" "src/*.mm")

# Exclude macOS helper implementation from the main binary sources
list(FILTER SOURCES EXCLUDE REGEX "process_helper_mac\\.cpp$")

# Exclude macOS Objective-C++ sources when not building for Apple platforms
if(NOT APPLE)
    list(FILTER SOURCES EXCLUDE REGEX "\\.mm$")
endif()

# Create executable
if(APPLE)
    add_executable(${PROJECT_NAME} MACOSX_BUNDLE
        ${SOURCES}
        ${DECKLINK_SOURCES}
    )
else()
    add_executable(${PROJECT_NAME} 
        ${SOURCES}
        ${DECKLINK_SOURCES}
    )
endif()

# Pass git commit hash as compile-time definition
target_compile_definitions(${PROJECT_NAME} PRIVATE
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
)

# Include Directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/render
    ${CMAKE_SOURCE_DIR}/src/decklink
    ${CMAKE_SOURCE_DIR}/src/cef
    ${CEF_ROOT}
    ${CMAKE_BINARY_DIR}
)

if(WIN32)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.dir/$<CONFIG>
    )
endif()

# --- Platform Configuration & Linking ---

if(WIN32)
    # Link CEF
    target_link_libraries(${PROJECT_NAME} PRIVATE libcef_dll_wrapper libcef)

    # Post-build copy steps for Windows
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

    target_link_directories(${PROJECT_NAME} PRIVATE ${CEF_ROOT}/Release)
    target_link_libraries(${PROJECT_NAME} PRIVATE 
        d3d11.lib 
        dxgi.lib
        d3dcompiler.lib
        dbghelp.lib
        libcef_dll_wrapper
        "${CEF_ROOT}/Release/libcef.lib"
        gdi32.lib
        user32.lib
        wininet.lib
    )

    # Unicode support for Win32
    add_definitions(-DUNICODE -D_UNICODE)

elseif(APPLE)
    # Link CEF wrapper and framework
    target_link_libraries(${PROJECT_NAME} PRIVATE libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    SET_EXECUTABLE_TARGET_PROPERTIES(${PROJECT_NAME})
    set_source_files_properties("src/core/MacWindow.mm" PROPERTIES COMPILE_FLAGS "-fobjc-arc")

    # Link Mac system frameworks (needed for CEF / DeckLink / License validation)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${COREFOUNDATION_FRAMEWORK})
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SECURITY_FRAMEWORK})
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${COREGRAPHICS_FRAMEWORK})
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${APPKIT_FRAMEWORK})
    
    # Configure bundle plists
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/src/cef/mac/Info.plist.in
    )
    
    # Output path for the main app bundle
    SET_CEF_TARGET_OUT_DIR()
    set(CEF_APP "${CEF_TARGET_OUT_DIR}/${PROJECT_NAME}.app")
    set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CEF_TARGET_OUT_DIR}")
    
    # Set CEF binary directory path for macOS post-build copy step
    set(CEF_BINARY_DIR "${CEF_ROOT}/$<CONFIGURATION>")

    # Copy CEF framework bundle to Contents/Frameworks/
    COPY_MAC_FRAMEWORK("${PROJECT_NAME}" "${CEF_BINARY_DIR}" "${CEF_APP}")

    # Define CEF Helper app suffixes since they are local to CEF subdirectory scope
    set(CEF_HELPER_APP_SUFFIXES
      ":::"
      " (Alerts):_alerts:alerts"
      " (GPU):_gpu:gpu"
      " (Plugin):_plugin:plugin"
      " (Renderer):_renderer:renderer"
    )

    # Build and bundle the standard CEF Helper processes
    foreach(_suffix_list ${CEF_HELPER_APP_SUFFIXES})
        string(REPLACE ":" ";" _suffix_list ${_suffix_list})
        list(GET _suffix_list 0 _name_suffix)
        list(GET _suffix_list 1 _target_suffix)
        list(GET _suffix_list 2 _plist_suffix)

        set(_helper_target "${PROJECT_NAME}_Helper${_target_suffix}")
        set(_helper_output_name "${PROJECT_NAME} Helper${_name_suffix}")

        set(_helper_info_plist "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_target_suffix}.plist")
        file(READ "${CMAKE_SOURCE_DIR}/src/cef/mac/helper-Info.plist.in" _plist_contents)
        string(REPLACE "\${EXECUTABLE_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
        string(REPLACE "\${PRODUCT_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
        string(REPLACE "\${BUNDLE_ID_SUFFIX}" "${_plist_suffix}" _plist_contents ${_plist_contents})
        file(WRITE ${_helper_info_plist} ${_plist_contents})

        # Helper executable target
        add_executable(${_helper_target} MACOSX_BUNDLE 
            "${CMAKE_SOURCE_DIR}/src/cef/process_helper_mac.cpp"
        )
        SET_EXECUTABLE_TARGET_PROPERTIES(${_helper_target})
        target_include_directories(${_helper_target} PRIVATE ${CEF_ROOT})
        add_dependencies(${_helper_target} libcef_dll_wrapper)
        target_link_libraries(${_helper_target} libcef_dll_wrapper ${CEF_STANDARD_LIBS})
        set_target_properties(${_helper_target} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ${_helper_info_plist}
            OUTPUT_NAME "${_helper_output_name}"
            RUNTIME_OUTPUT_DIRECTORY "${CEF_TARGET_OUT_DIR}"
        )

        add_dependencies(${PROJECT_NAME} "${_helper_target}")
        
        # Copy Helper app bundle to Contents/Frameworks/
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_TARGET_OUT_DIR}/${_helper_output_name}.app"
            "${CEF_APP}/Contents/Frameworks/${_helper_output_name}.app"
            COMMENT "Copying ${_helper_output_name}.app to main bundle..."
            VERBATIM
        )
    endforeach()

    # Copy shaders and config into the main bundle resources
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/render/shaders"
        "${CEF_APP}/Contents/Resources/shaders"
        COMMENT "Copying Shaders to App Bundle..."
        VERBATIM
    )
    
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config.json"
        "${CEF_TARGET_OUT_DIR}"
        COMMENT "Copying config.json next to App Bundle..."
        VERBATIM
    )
endif()
