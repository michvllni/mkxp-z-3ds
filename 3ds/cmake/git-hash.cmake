# SPDX-License-Identifier: GPL-2.0-or-later
# Writes OUT (git-hash.h) with the short git hash of ROOT, like meson's vcs_tag in src/meson.build.
# Run at every build (cmake -P); the file is only rewritten when the hash changed, so nothing
# recompiles otherwise. Arguments: -DROOT=<repository> -DOUT=<header> [-DGIT=<git executable>]
set(hash "unknown")
if(GIT)
  execute_process(COMMAND "${GIT}" rev-parse --short=7 HEAD WORKING_DIRECTORY "${ROOT}"
                  OUTPUT_VARIABLE out RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(rc EQUAL 0 AND out)
    set(hash "${out}")
  endif()
endif()
file(WRITE "${OUT}.tmp" "#ifndef MKXPZ_GIT_HASH\n#  define MKXPZ_GIT_HASH \"${hash}\"\n#endif\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
