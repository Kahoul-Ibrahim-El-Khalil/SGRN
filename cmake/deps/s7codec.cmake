# cmake/deps/s7codec.cmake
# ─────────────────────────────────────────────────────────────────────────────
# NOTE: s7codec is NOT a third-party dependency. It is SGRN's own codec
# library, located at sgrn/lib/codecs/s7codec/. The sgrn::s7codec target is
# declared there via sgrn/lib/codecs/s7codec/CMakeLists.txt.
#
# The staging.cmake install of s7codec headers uses SGRN_ROOT/sgrn/lib/codecs/s7codec/include.
# This file is kept as a no-op to maintain the include() chain.
# ─────────────────────────────────────────────────────────────────────────────
