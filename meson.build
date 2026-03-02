project(
  'npkg_pkg',
  ['c', 'cpp'],
  default_options: [
    'c_std=c11',
    'cpp_std=c++20',
    'warning_level=2',
  ],
)

pkg_inc = include_directories('include', 'src')

pkg_core = static_library(
  'pkg_core',
  [
    'src/tomlc17.c',
    'src/config.cpp',
    'src/group.cpp',
    'src/port.cpp',
    'src/lockfile.cpp',
    'src/resolver.cpp',
    'src/commands.cpp',
  ],
  include_directories: pkg_inc,
)

executable(
  'pkg',
  'src/apps/main.cpp',
  include_directories: pkg_inc,
  link_with: pkg_core,
  install: true,
  install_dir: '/usr/bin',
)
