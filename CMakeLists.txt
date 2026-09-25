cmake_minimum_required(VERSION 3.25)
project(kernel CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded) # the C++ runtime inside the exe: a released one runs without the VC++ redistributable
add_executable(kernel kernel.cpp)
target_compile_options(kernel PRIVATE /utf-8 /W4)
target_link_libraries(kernel PRIVATE windowsapp ole32 oleaut32)
