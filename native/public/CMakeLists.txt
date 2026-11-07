cmake_minimum_required(VERSION 3.25)
project(MarioPartyDS-online LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The C++ runtime is linked in, so the programs run without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The lobby server and relay. Windows only (Winsock, CNG).
add_executable(MarioPartyDS-server server/server.cpp src/secure.cpp)
target_include_directories(MarioPartyDS-server PRIVATE src)
target_link_libraries(MarioPartyDS-server PRIVATE ws2_32 bcrypt)
target_compile_options(MarioPartyDS-server PRIVATE /W4 /WX /utf-8)

# A client for the command line, for tests and by hand.
add_executable(lobby-client client/lobby_client.cpp src/lobby.cpp src/secure.cpp)
target_include_directories(lobby-client PRIVATE src)
target_link_libraries(lobby-client PRIVATE ws2_32 bcrypt)
target_compile_options(lobby-client PRIVATE /W4 /WX /utf-8)
