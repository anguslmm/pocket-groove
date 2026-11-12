# Host tests for the hardware spike's portable parts (src/hwspike: tilt maths,
# sample resampler / filter / level statistics, report formatting).
# Registered with ctest as test_hwspike.
set(HWS_SRC ${PG_ROOT}/src/hwspike)
add_executable(test_hwspike test_hwspike.cpp ${HWS_SRC}/tilt_math.cpp ${HWS_SRC}/report.cpp)
target_include_directories(test_hwspike PRIVATE ${HWS_SRC})
target_compile_options(test_hwspike PRIVATE ${PG_FLAGS})
add_test(NAME test_hwspike COMMAND test_hwspike WORKING_DIRECTORY ${PG_ROOT})
