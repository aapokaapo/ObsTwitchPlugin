cmake_minimum_required(VERSION 3.21)

project(ObsTwitchPlugin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBOBS REQUIRED libobs)
pkg_check_modules(OBS_FRONTEND REQUIRED obs-frontend-api)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

add_library(obstwitchplugin MODULE
    src/plugin-main.cpp
    src/twitchdockwidget.h
    src/twitchdockwidget.cpp
)

target_include_directories(obstwitchplugin PRIVATE
    ${LIBOBS_INCLUDE_DIRS}
    ${OBS_FRONTEND_INCLUDE_DIRS}
)

target_link_directories(obstwitchplugin PRIVATE
    ${LIBOBS_LIBRARY_DIRS}
    ${OBS_FRONTEND_LIBRARY_DIRS}
)

foreach(_obs_flags_var LIBOBS_CFLAGS_OTHER OBS_FRONTEND_CFLAGS_OTHER)
    if(DEFINED ${_obs_flags_var})
        list(FILTER ${_obs_flags_var} EXCLUDE REGEX "^-std=")
    endif()
endforeach()

target_compile_options(obstwitchplugin PRIVATE
    ${LIBOBS_CFLAGS_OTHER}
    ${OBS_FRONTEND_CFLAGS_OTHER}
)

target_link_libraries(obstwitchplugin PRIVATE
    ${LIBOBS_LIBRARIES}
    ${OBS_FRONTEND_LIBRARIES}
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

set_target_properties(obstwitchplugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "obstwitchplugin"
)
