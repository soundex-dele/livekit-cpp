include(CMakeFindDependencyMacro)
find_dependency(PkgConfig)
pkg_check_modules(LK_VPX REQUIRED IMPORTED_TARGET GLOBAL vpx)
if(NOT TARGET unofficial::libvpx::libvpx)
  add_library(unofficial::libvpx::libvpx INTERFACE IMPORTED GLOBAL)
  set_target_properties(unofficial::libvpx::libvpx PROPERTIES
    INTERFACE_LINK_LIBRARIES PkgConfig::LK_VPX)
endif()
