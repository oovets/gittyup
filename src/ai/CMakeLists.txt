add_library(
  ai
  AiReviewStore.cpp
  AiService.cpp
  CodebaseIndex.cpp
  EmbeddingClient.cpp
  FixApplicator.cpp
  FindingParser.cpp
  KnowledgeBase.cpp
  RepoAnalyzer.cpp
  TaskDispatcher.cpp)

target_link_libraries(ai conf Qt6::Core Qt6::Network Qt6::Sql)

set_target_properties(ai PROPERTIES AUTOMOC ON)
