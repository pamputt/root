# Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.

set(headers
    inc/CPyCppyy/API.h
    inc/CPyCppyy/Reflex.h
    inc/CPyCppyy/PyResult.h
    inc/CPyCppyy/CommonDefs.h
    inc/CPyCppyy/PyException.h
    inc/CPyCppyy/DispatchPtr.h
)

set(sources
    src/API.cxx
    src/CallContext.cxx
    src/Converters.cxx
    src/CPPClassMethod.cxx
    src/CPPConstructor.cxx
    src/CPPDataMember.cxx
    src/CPPExcInstance.cxx
    src/CPPFunction.cxx
    src/CPPInstance.cxx
    src/CPPMethod.cxx
    src/CPPOverload.cxx
    src/CPPScope.cxx
    src/CPPGetSetItem.cxx
    src/CPyCppyyModule.cxx
    src/CustomPyTypes.cxx
    src/Dispatcher.cxx
    src/DispatchPtr.cxx
    src/Executors.cxx
    src/LowLevelViews.cxx
    src/MemoryRegulator.cxx
    src/ProxyWrappers.cxx
    src/PyStrings.cxx
    src/Pythonize.cxx
    src/TemplateProxy.cxx
    src/PyException.cxx
    src/PyResult.cxx
    src/TupleOfInstances.cxx
    src/TypeManip.cxx
    src/Utility.cxx
)

file(RELATIVE_PATH PYTHONDIR_TO_LIBDIR "${CMAKE_INSTALL_FULL_PYTHONDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")

foreach(val RANGE ${how_many_pythons})
  list(GET python_include_dirs ${val} python_include_dir)
  list(GET python_under_version_strings ${val} python_under_version_string)
  list(GET python_version_strings ${val} python_version_string)

  set(libname cppyy${python_under_version_string})

  add_library(${libname} SHARED ${headers} ${sources})
  # Set the suffix to '.so' and the prefix to 'lib'
  set_target_properties(${libname} PROPERTIES  ${ROOT_LIBRARY_PROPERTIES})
  if(MSVC)
    target_link_libraries(${libname} PUBLIC cppyy_backend${python_under_version_string} ${PYTHON_LIBRARIES})
    set_target_properties(${libname} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    set_target_properties(${libname} PROPERTIES PREFIX "lib")
    set_target_properties(${libname} PROPERTIES SUFFIX ".pyd")
  elseif(APPLE)
    target_link_libraries(${libname} PUBLIC -Wl,-bind_at_load -Wl,-w -Wl,-undefined -Wl,dynamic_lookup cppyy_backend${python_under_version_string})
  else()
    target_link_libraries(${libname} PUBLIC -Wl,--unresolved-symbols=ignore-all cppyy_backend${python_under_version_string})
  endif()

  if(NOT MSVC)
    target_compile_options(${libname} PRIVATE
      -Wno-shadow -Wno-strict-aliasing)
  endif()
  if(NOT "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" AND NOT MSVC)
    target_compile_options(${libname} PRIVATE
      -Wno-unused-but-set-parameter)
  endif()

  # Disables warnings coming from PyCFunction casts
  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND ${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER_EQUAL 8)
    target_compile_options(${libname} PRIVATE -Wno-cast-function-type)
  endif()

  # Disables warnings due to new field tp_vectorcall in Python 3.8
  if(NOT MSVC AND ${python_version_string} VERSION_GREATER_EQUAL "3.8")
    target_compile_options(${libname} PRIVATE -Wno-missing-field-initializers)
  endif()

  target_include_directories(${libname}
     SYSTEM PUBLIC ${python_include_dir})

  target_include_directories(${libname}
     PRIVATE
        ${CMAKE_BINARY_DIR}/ginclude
     PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  )

  set_property(GLOBAL APPEND PROPERTY ROOT_EXPORTED_TARGETS ${libname})

  # Install library
  install(TARGETS ${libname} EXPORT ${CMAKE_PROJECT_NAME}Exports
                             RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT libraries
                             LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT libraries
                             ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT libraries)
  if (NOT MSVC AND NOT CMAKE_INSTALL_LIBDIR STREQUAL CMAKE_INSTALL_PYTHONDIR)
    # add a symlink to ${libname} in CMAKE_INSTALL_PYTHONDIR
    set(LIB_FILE_NAME ${CMAKE_SHARED_LIBRARY_PREFIX}${libname}.so)
    install(CODE "file(CREATE_LINK ${PYTHONDIR_TO_LIBDIR}/${LIB_FILE_NAME}
      \$ENV{DESTDIR}${CMAKE_INSTALL_FULL_PYTHONDIR}/${LIB_FILE_NAME} SYMBOLIC)")
  endif()
endforeach()

file(COPY ${headers} DESTINATION ${CMAKE_BINARY_DIR}/include/CPyCppyy)
install(FILES ${headers}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/CPyCppyy
        COMPONENT headers)
