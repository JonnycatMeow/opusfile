find_package(Ogg CONFIG)
if(NOT TARGET Ogg::ogg)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(Ogg REQUIRED IMPORTED_TARGET ogg)
  add_library(Ogg::ogg ALIAS PkgConfig::Ogg)
endif()
