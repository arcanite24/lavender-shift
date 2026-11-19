# cmake -DSOURCES=<a;b;...> -DDESTINATION=<file> -P join_files.cmake
# Writes the sources one after another into DESTINATION, each ending in a
# newline.
set(_joined "")
foreach(_source IN LISTS SOURCES)
    file(READ "${_source}" _text)
    string(APPEND _joined "${_text}")
    if(NOT _text MATCHES "\n$")
        string(APPEND _joined "\n")
    endif()
endforeach()
file(WRITE "${DESTINATION}.tmp" "${_joined}")
file(RENAME "${DESTINATION}.tmp" "${DESTINATION}")
