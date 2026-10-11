# Writes a C++ source that builds files into the program, each as an Embedded (src/embedded.hpp).
# Runs at build time: cmake -DOUTPUT=<file.cpp> "-DFILES=NAME=path;NAME=path" -P embed.cmake
set(source "#include \"embedded.hpp\"\n")
foreach(entry IN LISTS FILES)
  string(REGEX MATCH "^([A-Z_]+)=(.*)$" _ "${entry}")
  set(name "${CMAKE_MATCH_1}")
  file(READ "${CMAKE_MATCH_2}" hex HEX)
  # 32 bytes per line, as 0x.. values.
  string(REGEX REPLACE "(................................................................)" "\\1\n" hex "${hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  string(APPEND source "static const unsigned char ${name}_DATA[] = {\n${hex}};\n")
  string(APPEND source "const Embedded ${name} = {${name}_DATA, sizeof(${name}_DATA)};\n")
endforeach()
file(WRITE "${OUTPUT}" "${source}")
