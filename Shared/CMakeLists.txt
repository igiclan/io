# pf_shared: platform-neutral core used by Server, Client, tools and tests.
add_library(pf_shared STATIC
    Common/Error.cpp
    Common/ByteIo.cpp
    Common/BufferPool.cpp
    Common/Platform.cpp
    Models/UsbTypes.cpp
    Models/Descriptors.cpp
    Models/DescriptorBuilder.cpp
    Models/DeviceIdentity.cpp
    Models/ModeDetector.cpp
    Models/SessionState.cpp
    Protocol/Protocol.cpp
)
target_include_directories(pf_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pf_shared
    PUBLIC pf_options pf_asio nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(pf_shared PUBLIC psapi crypt32 bcrypt)
endif()
