# Applies PATCH_FILE to the current directory, unless it is already applied (the patch step re-runs on reconfigure).
# Usage: cmake -DPATCH_FILE=<file> -P apply-patch.cmake
execute_process(COMMAND git apply --reverse --check ${PATCH_FILE} RESULT_VARIABLE already_applied ERROR_QUIET)
if (NOT already_applied EQUAL 0)
    execute_process(COMMAND git apply ${PATCH_FILE} RESULT_VARIABLE apply_result)
    if (NOT apply_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${PATCH_FILE}")
    endif ()
endif ()
