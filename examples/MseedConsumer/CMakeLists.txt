cmake_minimum_required(VERSION 3.13)
project(MseedConsumer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 REQUIRED COMPONENTS Core Network Widgets)

add_executable(MseedConsumer WIN32
    main.cpp
    ConsumerWindow.h
    ConsumerWindow.cpp
    MseedConsumer.h
    MseedConsumer.cpp
)

target_link_libraries(MseedConsumer PRIVATE Qt5::Core Qt5::Network Qt5::Widgets)

if(MSVC)
    target_compile_options(MseedConsumer PRIVATE /utf-8)
endif()
