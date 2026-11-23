# Run the existing loader suite with each fixture in its intended directory.
set(layout "${CMAKE_ARGV3}")
set(program "${CMAKE_ARGV4}")
set(legacy "${CMAKE_ARGV5}")
set(cwd_only "${CMAKE_ARGV6}")
set(package "${CMAKE_ARGV7}")
get_filename_component(original_bin "${program}" DIRECTORY)
get_filename_component(original_build "${original_bin}" DIRECTORY)
get_filename_component(program_name "${program}" NAME)

file(MAKE_DIRECTORY "${layout}/bin" "${layout}/plugins"
     "${layout}/package/bin/plugins" "${layout}/cwd")
file(COPY "${program}" "${legacy}" DESTINATION "${layout}/bin")
file(COPY "${cwd_only}" DESTINATION "${layout}/cwd")
file(COPY "${package}" DESTINATION "${layout}/package/bin/plugins")
set(index 8)
while(index LESS CMAKE_ARGC)
  file(COPY "${CMAKE_ARGV${index}}" DESTINATION "${layout}/plugins")
  math(EXPR index "${index} + 1")
endwhile()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "TOAD_ROOT=${layout}/package"
          --modify "PATH=path_list_prepend:${original_bin}"
          --modify "LD_LIBRARY_PATH=path_list_prepend:${original_build}/lib"
          --modify "DYLD_LIBRARY_PATH=path_list_prepend:${original_build}/lib"
          -- "${layout}/bin/${program_name}"
  WORKING_DIRECTORY "${layout}/cwd"
  COMMAND_ERROR_IS_FATAL ANY)
