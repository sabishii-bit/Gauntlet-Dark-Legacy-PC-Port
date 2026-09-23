find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
find_package(Stb REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(engine STATIC
    core/Types.h
    core/SpecialMembers.h
    core/Error.h
    core/Log.h
    core/Log.cpp
    core/Assert.h
    core/Assert.cpp
    core/Clock.h
    core/Clock.cpp
    core/Strings.h
    core/Strings.cpp
    math/Math.h
    io/ByteReader.h
    io/File.h
    io/File.cpp
    io/AssetLocator.h
    io/AssetLocator.cpp
    platform/Window.h
    platform/Input.h
    platform/Input.cpp
    platform/Paths.h
    platform/Paths.cpp
    platform/GlfwWindow.h
    platform/GlfwWindow.cpp
    render/RenderTypes.h
    render/VulkanHandles.h
    render/Mesh.h
    render/Image.h
    render/Image.cpp
    render/ImmediateBatch.h
    render/ImmediateBatch.cpp
    render/RenderDevice.h
    render/DebugTextures.h
    render/DebugTextures.cpp
    render/vulkan/VulkanCommon.h
    render/vulkan/VulkanCommon.cpp
    render/vulkan/VmaImplementation.cpp
    render/vulkan/VulkanContext.h
    render/vulkan/VulkanContext.cpp
    render/vulkan/VulkanSwapchain.h
    render/vulkan/VulkanSwapchain.cpp
    render/vulkan/VulkanPipeline.h
    render/vulkan/VulkanPipeline.cpp
    render/vulkan/VulkanTexture.h
    render/vulkan/VulkanTexture.cpp
    render/vulkan/VulkanRenderDevice.h
    render/vulkan/VulkanRenderDevice.cpp
    codec/Lzss.h
    codec/Lzss.cpp
    codec/AviReader.h
    codec/AviReader.cpp
    codec/VqVideoDecoder.h
    codec/VqVideoDecoder.cpp
    codec/DspAdpcm.h
    codec/DspAdpcm.cpp
    codec/AdsAudio.h
    codec/AdsAudio.cpp
    codec/MoviePlayback.h
    codec/MoviePlayback.cpp
    audio/AudioStream.h
    audio/AudioStream.cpp
    audio/AudioMixer.h
    audio/AudioMixer.cpp
    audio/AudioDevice.h
    audio/AudioDevice.cpp
    audio/MiniaudioImplementation.cpp
    audio/SoundClip.h
    audio/SoundPlayer.h
    audio/SoundPlayer.cpp
    audio/StreamSource.h
    audio/AdsStream.h
    audio/AdsStream.cpp
    audio/StreamPlaylist.cpp
    audio/StreamPlaylist.h
    assets/PngImage.h
    assets/PngImage.cpp
    assets/StbImageImplementation.cpp
    assets/TextureSet.h
    assets/TextureSet.cpp
    assets/BitmapFont.h
    assets/BitmapFont.cpp
    assets/WavFile.h
    assets/WavFile.cpp
    assets/SoundSet.h
    assets/SoundSet.cpp
    assets/ObjModel.h
    assets/ObjModel.cpp
    assets/ModelSet.h
    assets/ModelSet.cpp
    assets/ItemArchive.h
    assets/ItemArchive.cpp
    assets/ParticleTemplate.h
    assets/ParticleTemplateJson.h
    assets/ParticleTemplate.cpp
    assets/AnimationSet.h
    assets/AnimationSet.cpp
    assets/WorldData.h
    assets/WorldData.cpp
    assets/MessageTable.h
    assets/MessageTable.cpp
    assets/StringTable.h
    assets/StringTable.cpp
    assets/WorldLayout.h
    assets/WorldLayout.cpp
    ui/Canvas.h
    ui/Canvas.cpp
    ui/TextPainter.h
    ui/TextPainter.cpp
    ui/ModelSprite.h
    ui/ModelSprite.cpp
    world/AmbientDimmer.h
    world/AmbientDimmer.cpp
    world/WorldLighting.h
    world/WorldLighting.cpp
    world/WorldCamera.h
    world/WorldCamera.cpp
    world/WorldScene.h
    world/WorldScene.cpp
    world/TextureAnimator.h
    world/TextureAnimator.cpp
    world/WorldAnimator.h
    world/WorldAnimator.cpp
    world/ParticleSystem.h
    world/ParticleSystem.cpp
    world/ParticleField.h
    world/ParticleField.cpp
    world/TreeParticles.h
    world/TreeParticles.cpp
    world/WorldCollision.h
    world/WorldCollision.cpp
    world/TreeModel.h
    world/TreeModel.cpp
    world/AnimationPlayer.h
    world/AnimationPlayer.cpp
    world/TreePose.h
    world/TreePose.cpp
    app/Application.h
    app/Application.cpp)

add_library(gdl::engine ALIAS engine)

target_include_directories(engine PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(engine SYSTEM PRIVATE ${MINIAUDIO_INCLUDE_DIR} ${Stb_INCLUDE_DIR})

target_link_libraries(engine
    PUBLIC
        glm::glm
    PRIVATE
        glfw
        nlohmann_json::nlohmann_json
        Vulkan::Headers
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        Threads::Threads
        ${CMAKE_DL_LIBS})

if(UNIX AND NOT APPLE)
    target_link_libraries(engine PRIVATE m)
endif()

target_compile_definitions(engine
    PUBLIC
        GLM_FORCE_RADIANS
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL
    PRIVATE
        VK_NO_PROTOTYPES
        VMA_STATIC_VULKAN_FUNCTIONS=0
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1
        GDL_ENABLE_VULKAN_VALIDATION=$<BOOL:${GDL_ENABLE_VULKAN_VALIDATION}>)

gdl_apply_compiler_options(engine)
set_target_properties(engine PROPERTIES FOLDER "engine")

get_target_property(_engine_sources engine SOURCES)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${_engine_sources})
