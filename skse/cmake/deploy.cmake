# Copies the built plugin into a Mod Organizer mod folder (development). A running Skyrim holds the
# DLL, so a failed copy is only a warning: the build itself succeeded.
foreach(file IN ITEMS "${DLL}" "${PDB}")
	execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${file}" "${DEST}/" RESULT_VARIABLE result)
	if(NOT result EQUAL 0)
		message(WARNING "SkyCraft: couldn't copy ${file} to ${DEST} (is Skyrim running?)")
	endif()
endforeach()
