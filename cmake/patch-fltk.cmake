# Run in FLTK's source folder (FetchContent's PATCH_COMMAND). FLTK 1.4.5's Wayland build adds its
# build folder to the compiler flags without quotes, which breaks when that path has a space, so
# add it as an include folder instead. Running it again changes nothing.
set(file src/CMakeLists.txt)
file(READ ${file} text)
string(REPLACE [[set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I${CMAKE_CURRENT_BINARY_DIR}")]]
               [[include_directories(${CMAKE_CURRENT_BINARY_DIR})]] text "${text}")
file(WRITE ${file} "${text}")
