# G-Diffuser's N64 audio path for Quest.  Keep the same HLE fallback and cxd4 LLE RSP core as
# desktop; only the final Android audio sink is a separate Quest integration concern.

set(GDX_CXD4_DIR "${GDX_SOURCE_DIR}/port/rsp/cxd4")

add_library(cxd4_rsp STATIC
    "${GDX_CXD4_DIR}/gdx_rsp_driver.c"
    "${GDX_CXD4_DIR}/su.c"
    "${GDX_CXD4_DIR}/vu/add.c"
    "${GDX_CXD4_DIR}/vu/divide.c"
    "${GDX_CXD4_DIR}/vu/logical.c"
    "${GDX_CXD4_DIR}/vu/multiply.c"
    "${GDX_CXD4_DIR}/vu/select.c"
    "${GDX_CXD4_DIR}/vu/vu.c")

set_target_properties(cxd4_rsp PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)
target_include_directories(cxd4_rsp PUBLIC "${GDX_CXD4_DIR}")
target_compile_definitions(cxd4_rsp PRIVATE SP_EXECUTE_LOG=1 _CRT_SECURE_NO_WARNINGS=1)
# The debug APK otherwise leaves the instruction-by-instruction LLE RSP interpreter at -O0.
# Audio runs continuously, so optimize this hot core while preserving its alias/wrap semantics.
target_compile_options(cxd4_rsp PRIVATE -O3 -w -fno-strict-aliasing -fwrapv)
