set(LLVM_LINK_COMPONENTS
  Support
)

add_clang_executable(umler Umler.cpp)

target_link_libraries(umler
  sqlite3
  clangAST
  clangASTMatchers
  clangBasic
  clangFrontend
  clangTooling
)
