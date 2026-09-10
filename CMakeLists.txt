cmake_minimum_required(VERSION 3.13)
project(MiniCollection VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Network Sql)

# libmseed（miniSEED3 打包/解析）
set(MSEED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mseed")
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(MSEED_LIB "${MSEED_DIR}/libs/libmseed.lib")
    else()
        set(MSEED_LIB "${MSEED_DIR}/libs/x32/libmseed.lib")
    endif()
else()
    set(MSEED_LIB "${MSEED_DIR}/libs_linux/libmseed.a")
endif()

set(MINISRC
    src/main.cpp
    src/FrameParser.h    src/FrameParser.cpp
    src/StatusWindow.h   src/StatusWindow.cpp
    src/AppConfig.h      src/AppConfig.cpp
    src/CallManage.h     src/CallManage.cpp
    src/HttpMgr.h        src/HttpMgr.cpp
    src/LogManage.h      src/LogManage.cpp
    src/ConnectionPool.h src/ConnectionPool.cpp
    src/QSqLiteHelper.h  src/QSqLiteHelper.cpp
    src/TcpConnection.h  src/TcpConnection.cpp
    src/TcpDataManager.h src/TcpDataManager.cpp
    src/MDataPickder.h   src/MDataPickder.cpp
    src/MDataWriter.h    src/MDataWriter.cpp
    src/MseedFileHandler.h src/MseedFileHandler.cpp
    src/picker.h         src/picker.cpp
    src/is_compliant_event.h
    src/Commdef.h
)

add_executable(MiniCollection WIN32 ${MINISRC})

target_include_directories(MiniCollection PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MSEED_DIR}/include
)

if(MSVC)
    # 源码为 UTF-8（含中文注释/字符串）
    target_compile_options(MiniCollection PRIVATE /utf-8)
endif()

target_link_libraries(MiniCollection PRIVATE
    Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network Qt5::Sql
    ${MSEED_LIB}
)

# Linux：libmseed.a 依赖 libm 的数学函数
if(UNIX AND NOT APPLE)
    target_link_libraries(MiniCollection PRIVATE m)
    # 预编译的 libmseed.a 未加 -fPIC（位置相关），而新版 GCC 默认生成 PIE 可执行文件，
    # 链接时报 "relocation R_X86_64_32 ... recompile with -fPIE"。
    # 关闭 PIE 即可正常链接。
    target_link_options(MiniCollection PRIVATE -no-pie)
endif()

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    # openblas 依赖 dll 随程序输出
    add_custom_command(TARGET MiniCollection POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MSEED_DIR}/libs/libopenblas.dll" $<TARGET_FILE_DIR:MiniCollection>)
endif()

# ---------- 看门狗（独立小进程，守护 MiniCollection.exe） ----------
add_executable(MiniWatchdog
    src/watchdog/main.cpp
    src/watchdog/watchdog.h
    src/watchdog/watchdog.cpp
)

target_compile_definitions(MiniWatchdog PRIVATE
    $<$<PLATFORM_ID:Windows>:UNICODE _UNICODE>)

if(MSVC)
    target_compile_options(MiniWatchdog PRIVATE /utf-8)
endif()

target_link_libraries(MiniWatchdog PRIVATE
    Qt5::Core Qt5::Network
)
