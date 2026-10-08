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
    Common/Config.cpp
    Models/DeviceInfo.cpp
    Protocol/Protocol.cpp
    Logging/Logger.cpp
    Diagnostics/Metrics.cpp
    Crypto/Crypto.cpp
    Crypto/TokenStore.cpp
    Crypto/SecretStore.cpp
    Network/Transport.cpp
    Network/FrameConnection.cpp
    Licensing/License.cpp
)
target_include_directories(pf_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pf_shared
    PUBLIC pf_options pf_asio nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(pf_shared PUBLIC psapi crypt32 bcrypt)
endif()
