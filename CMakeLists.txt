cmake_minimum_required(VERSION 3.21)

# Nome do projeto = nome do arquivo .dll
project(Overgeared VERSION 0.1.0 LANGUAGES CXX)

# Baseado no template oficial SKSE_Template_HelloWorld (SkyrimScripting)
find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES plugin.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)
