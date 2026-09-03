# Turns a file into a C source holding its bytes:
#   cmake -DINPUT=<file> -DOUTPUT=<file.c> -DSYMBOL=<name> -P embed.cmake
# defines `const unsigned char SYMBOL[]` and `const size_t SYMBOL_size`.
file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x[0-9a-f][0-9a-f],){24}" "\\0\n" bytes "${bytes}")
file(WRITE "${OUTPUT}" "#include <stddef.h>\nconst unsigned char ${SYMBOL}[] = {\n${bytes}\n};\nconst size_t ${SYMBOL}_size = sizeof(${SYMBOL});\n")
