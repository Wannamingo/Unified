# SPDX-License-Identifier: MIT
_poly_headers = custom_target('unified-libpoly-header',
  input : meson.project_source_root() / 'src/compiler/poly_generated/libpoly.h',
  output : 'libpoly.h',
  command : [prog_python, '-c', 'import shutil,sys;shutil.copyfile(sys.argv[1],sys.argv[2])',
    '@INPUT@', '@OUTPUT@'])
