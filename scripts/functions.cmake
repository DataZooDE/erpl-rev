# SAP NW RFC SDK resolution — same approach as DataZooDE/erpl
# (erpl/scripts/functions.cmake): locate the SDK by relative path from the repo
# root and find the sapnwrfc + sapucum libraries under it.

function(find_sap_libraries LIB_LIST_VAR SAPNWRFC_HOME_PATH SAPNWRFC_LIB_NAME SAPUCUM_LIB_NAME)
      find_library(SAPNWRFC_LIB ${SAPNWRFC_LIB_NAME} PATHS ${SAPNWRFC_HOME_PATH}/lib)
      find_library(SAPUCUM_LIB ${SAPUCUM_LIB_NAME} PATHS ${SAPNWRFC_HOME_PATH}/lib)

      if(NOT SAPNWRFC_LIB)
            message(FATAL_ERROR "Could not find ${SAPNWRFC_LIB_NAME} library under ${SAPNWRFC_HOME_PATH}/lib")
      endif()

      if(NOT SAPUCUM_LIB)
            message(FATAL_ERROR "Could not find ${SAPUCUM_LIB_NAME} library under ${SAPNWRFC_HOME_PATH}/lib")
      endif()

      list(APPEND SAP_LIBS ${SAPNWRFC_LIB} ${SAPUCUM_LIB})
      set(${LIB_LIST_VAR} ${SAP_LIBS} PARENT_SCOPE)
endfunction()

#---------------------------------------------------------------------------------------

# Resolve SAPNWRFC_HOME from the repo-local nwrfcsdk/<platform> dir (overridable
# with -DSAPNWRFC_HOME=...) and populate SAPNWRFC_LIB_FILES. The SDK itself is
# gitignored — drop it under nwrfcsdk/linux (see README).
function(default_linux_libraries)
   if(NOT SAPNWRFC_HOME)
      get_filename_component(SAPNWRFC_HOME ${CMAKE_CURRENT_SOURCE_DIR}/nwrfcsdk/linux ABSOLUTE)
   endif()
   if(NOT EXISTS "${SAPNWRFC_HOME}/include/sapnwrfc.h")
      message(FATAL_ERROR "NW RFC SDK not found at ${SAPNWRFC_HOME}; place it under nwrfcsdk/linux or set -DSAPNWRFC_HOME=...")
   endif()
   find_sap_libraries(SAPNWRFC_LIB_FILES ${SAPNWRFC_HOME} "sapnwrfc" "sapucum")
   set(SAPNWRFC_HOME ${SAPNWRFC_HOME} PARENT_SCOPE)
   set(SAPNWRFC_LIB_FILES ${SAPNWRFC_LIB_FILES} PARENT_SCOPE)
endfunction()
