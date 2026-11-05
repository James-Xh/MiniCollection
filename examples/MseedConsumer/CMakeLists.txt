cmake_minimum_required(VERSION 3.13)
project(MseedConsumer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 REQUIRED COMPONENTS Core Network Widgets)

set(CONSUMER_SOURCES
    main.cpp
    ConsumerWindow.h
    ConsumerWindow.cpp
    MseedConsumer.h
    MseedConsumer.cpp
    resources.qrc
)

if(WIN32)
    list(APPEND CONSUMER_SOURCES app.rc)
endif()

add_executable(MseedConsumer WIN32 ${CONSUMER_SOURCES})

# 对外发布的程序文件名；CMake 内部目标名仍为 MseedConsumer，便于看门狗和构建脚本引用。
set_target_properties(MseedConsumer PROPERTIES
    OUTPUT_NAME "地面微震采集控制台"
)

target_include_directories(MseedConsumer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(MseedConsumer PRIVATE Qt5::Core Qt5::Network Qt5::Widgets)

if(MSVC)
    target_compile_options(MseedConsumer PRIVATE /utf-8)
endif()

# ---------- 控制台看门狗（异常退出后自动重启 MseedConsumer） ----------
add_executable(MseedConsumerWatchdog WIN32 watchdog_main.cpp)

target_link_libraries(MseedConsumerWatchdog PRIVATE Qt5::Core)

if(MSVC)
    target_compile_options(MseedConsumerWatchdog PRIVATE /utf-8)
endif()
