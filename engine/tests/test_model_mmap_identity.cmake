set(prefix NIYAH_MMAP_MODEL)
if(NOT DEFINED ENV{NIYAH_MMAP_MODEL_PATH} AND NOT DEFINED ENV{NIYAH_MMAP_MODEL_SHA256})
    set(prefix NIYAH_CASPER_MODEL)
endif()

if("$ENV{${prefix}_PATH}" STREQUAL "" OR
   "$ENV{${prefix}_SHA256}" STREQUAL "")
    message("SKIP: external model is not configured")
    return()
endif()

execute_process(COMMAND "${TEST_EXECUTABLE}" RESULT_VARIABLE result)
if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR "Valid external model failed: ${result}")
endif()

foreach(bad_hash IN ITEMS
    "0000000000000000000000000000000000000000000000000000000000000000"
    "invalid"
    "gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env
        "${prefix}_SHA256=${bad_hash}" "${TEST_EXECUTABLE}"
        RESULT_VARIABLE result)
    if(NOT "${result}" STREQUAL "1")
        message(FATAL_ERROR "Invalid hash must return 1, got: ${result}")
    endif()
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    --unset=${prefix}_PATH "${TEST_EXECUTABLE}"
    RESULT_VARIABLE result)
if(NOT "${result}" STREQUAL "77")
    message(FATAL_ERROR "Missing model must return skip code 77, got: ${result}")
endif()
message("External model identity regression checks passed")
