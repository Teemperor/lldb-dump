get_property(LLDB_ALL_PLUGINS GLOBAL PROPERTY LLDB_PLUGINS)

add_lldb_unittest(EditlineTests
  EditlineTest.cpp

  LINK_LIBS
      lldbBase
      lldbBreakpoint
      lldbCore
      lldbDataFormatters
      lldbExpression
      lldbHost
      lldbInitialization
      lldbInterpreter
      lldbSymbol
      lldbTarget
      lldbUtility
      ${LLDB_ALL_PLUGINS}
      ${host_lib}

  )
