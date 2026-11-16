# SPDX-License-Identifier: MIT
_poly = mesa_src / 'poly'
subdir('cl')
_poly_lib = static_library('unified_libpoly',
  files(_poly / 'nir/poly_nir_lower_gs.c', _poly / 'nir/poly_nir_lower_sysvals.c',
        _poly / 'nir/poly_nir_lower_vs.c',
        meson.project_source_root() / 'src/compiler/poly_generated/libpoly.cpp'),
  _poly_headers,
  include_directories : [inc_include, inc_src],
  c_args : [mesa_c_args, '-Wno-c2x-extensions'],
  cpp_args : mesa_c_args,
  dependencies : [idep_nir, idep_mesautil],
  gnu_symbol_visibility : 'hidden')
idep_unified_poly = declare_dependency(
  link_with : _poly_lib, sources : _poly_headers,
  include_directories : include_directories('.'))
