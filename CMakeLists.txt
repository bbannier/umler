set(LLVM_LINK_COMPONENTS
  Support
)

add_clang_executable(umler
  Report.cpp
  DB.cpp
  Umler.cpp
)

target_link_libraries(umler
  PRIVATE
  sqlite3
  clangAST
  clangASTMatchers
  clangBasic
  clangFrontend
  clangTooling
)

install(TARGETS umler RUNTIME DESTINATION bin)
