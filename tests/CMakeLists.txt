# Tests for MTL-Encode-SDK
# Structure: unit/ (no MTL runtime), integration/ (needs MTL), scripts/

# ---- Unit tests (link SDK, no MTL runtime required for sdp/encode) ----
add_executable(sdp_test unit/sdp_test.cpp)
target_link_libraries(sdp_test PRIVATE mtl_sdk)

add_executable(sdp_to_session_test unit/sdp_to_session_test.cpp)
target_link_libraries(sdp_to_session_test PRIVATE mtl_sdk)

add_executable(encode_format_test unit/encode_format_test.cpp)
target_link_libraries(encode_format_test PRIVATE mtl_sdk encode_sdk)

add_executable(encode_reconfigure_test unit/encode_reconfigure_test.cpp)
target_link_libraries(encode_reconfigure_test PRIVATE mtl_sdk encode_sdk)

# ---- Integration tests (need MTL at runtime) ----
add_executable(ptp_behavior_test integration/ptp_behavior_test.cpp)
target_link_libraries(ptp_behavior_test PRIVATE mtl_sdk)
