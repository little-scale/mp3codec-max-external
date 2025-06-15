cmake_minimum_required(VERSION 3.19)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../max-sdk-base/script/max-pretarget.cmake)

include_directories( 
    "${MAX_SDK_INCLUDES}"
    "${MAX_SDK_MSP_INCLUDES}"
    "/opt/homebrew/opt/lame/include"
)

file(GLOB PROJECT_SRC "*.h" "*.c" "*.cpp")
add_library(${PROJECT_NAME} MODULE ${PROJECT_SRC})

target_link_libraries(${PROJECT_NAME} PRIVATE 
    "/opt/homebrew/opt/lame/lib/libmp3lame.a"
)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../max-sdk-base/script/max-posttarget.cmake)