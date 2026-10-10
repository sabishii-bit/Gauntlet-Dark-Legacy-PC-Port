find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Optional room-service implementation shared by the application and process tests.
# The protocol/session state machines stay in the dependency-light game target.
add_library(room_service STATIC RoomClient.cpp RoomSession.cpp)
add_library(gdl::room_service ALIAS room_service)
target_link_libraries(room_service PUBLIC gdl::engine_base
    PRIVATE httplib::httplib nlohmann_json::nlohmann_json Threads::Threads)
gdl_apply_compiler_options(room_service)
set_target_properties(room_service PROPERTIES FOLDER "game")
