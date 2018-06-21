add_lldb_unittest(EditlineTests
  EditlineTest.cpp

  LINK_LIBS
    liblldb
    lldbCore
    lldbHost
    lldbUtility
  )
