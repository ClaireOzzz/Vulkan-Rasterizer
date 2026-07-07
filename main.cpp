/*
 * Copyright (c) 2023-2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2023-2025, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

// This example demonstrates a minimal Vulkan application using the NVIDIA
// Vulkan utility libraries. It creates a window displaying a single colored
// pixel that animates through the HSV color space.

#define VMA_IMPLEMENTATION
#define TINYGLTF_IMPLEMENTATION         // Implementation of the GLTF loader library
#define STB_IMAGE_IMPLEMENTATION        // Implementation of the image loading library
#define STB_IMAGE_WRITE_IMPLEMENTATION  // Implementation of the image writing library
// #include <tiny_gltf.h>

#include <backends/imgui_impl_vulkan.h>
#include <nvapp/application.hpp>
#include <nvapp/elem_profiler.hpp>
#include <nvapp/elem_logger.hpp>
#include <nvapp/elem_default_menu.hpp>
#include <nvapp/elem_default_title.hpp>
#include <nvutils/logger.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/context.hpp>
#include <nvvk/debug_util.hpp>
#include <nvvk/default_structs.hpp>
#include <nvvk/resource_allocator.hpp>
#include <nvvk/sampler_pool.hpp>
#include <nvvk/staging.hpp>
#include <nvvk/profiler_vk.hpp>
#include <nvutils/parameter_parser.hpp>
// Rasterization
#include <nvslang/slang.hpp>               // Slang compiler
#include <nvshaders_host/sky.hpp>          // Sky shader
#include <nvshaders_host/tonemapper.hpp>   // Tonemapper shader
#include <nvvk/gbuffers.hpp>               // GBuffer management
#include <nvvk/formats.hpp>                // Finding Vulkan formats utilities
#include <nvutils/camera_manipulator.hpp>


#include "common/gltf_utils.hpp"
#include "common/path_utils.hpp"
#include "common/utils.hpp"
#include "shaders/shaderio.h"

// Pre-compiled shaders
#include "_autogen/sky_simple.slang.h"  // from nvpro_core2
#include "_autogen/tonemapper.slang.h"  //   "    "
#include "_autogen/foundation.slang.h"  // Local shader


class SampleElement : public nvapp::IAppElement
{
public:
  struct Info
  {
    nvutils::ProfilerManager*   profilerManager{};
    nvutils::ParameterRegistry* parameterRegistry{};
  };


  SampleElement(const Info& info)
      : m_info(info)
  {
    // let's add a command-line option to toggle animation
    m_info.parameterRegistry->add({"animate"}, &m_animate);
  }

  ~SampleElement() override = default;

  void onAttach(nvapp::Application* app) override
  {
    m_app                                = app;
    VmaAllocatorCreateInfo allocatorInfo = {
        .flags          = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = app->getPhysicalDevice(),
        .device         = app->getDevice(),
        .instance       = app->getInstance(),
    };

    // Initialize core components
    NVVK_CHECK(m_alloc.init(allocatorInfo));
    // The VMA allocator is used for all allocations, the staging uploader will use it for staging buffers and images
    m_stagingUploader.init(&m_alloc, true);

#if 0
    // VMA might report memory leaks for example:
    // "UNFREED ALLOCATION; Offset: 1158736; Size: 16; UserData: 0000000000000000; Name: allocID: 45; Type: BUFFER; Usage: 131107"
    // Then look for the leak name: "allocID: 45" and feed that ID to the following function.
    m_alloc.setLeakID(45);
    // You should get a breakpoint at the creation of the resource that was leaked.
#endif

    // Setting up the Slang compiler for hot reload shader
    m_slangCompiler.addSearchPaths(nvsamples::getShaderDirs());
    m_slangCompiler.defaultTarget();
    m_slangCompiler.defaultOptions();
    m_slangCompiler.addOption({slang::CompilerOptionName::DebugInformation,
                               {slang::CompilerOptionValueKind::Int, SLANG_DEBUG_INFO_LEVEL_MAXIMAL}});

    m_samplerPool.init(app->getDevice());
    VkSampler linearSampler{}; //null state, 
    NVVK_CHECK(m_samplerPool.acquireSampler(linearSampler)); //by default, bilinear sampling assigned to linear sampler
    NVVK_DBG_NAME(linearSampler);

    // create G buffers
    nvvk::GBufferInitInfo gBufferInit{
      .allocator    = &m_alloc,
      .colorFormats = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM}, // RT, tonemapped RT
      .depthFormat  = nvvk::findDepthFormat(m_app->getPhysicalDevice()),
      .imageSampler = linearSampler,
      .descriptorPool = m_app->getTextureDescriptorPool(),
    };

    m_gBuffers.init(gBufferInit);

    // load gltf models and textures (as m_textures), ready them for GPU read
    createScene();

    // set up descriptor set shape for textures, ready to bind
    createGraphicsDescriptorSetLayout();

    createGraphicsPipelineLayout();

    compileAndCreateGraphicsShaders(); // hot reload and compile slang shaders

    // m_textures are finally written into the descriptor_set
    updateTextures();

    // Initialize the Sky with the pre-compiled shader
    m_skySimple.init(&m_alloc, std::span(sky_simple_slang));

    // Initialize the tonemapper also with proe-compiled shader
    // m_tonemapper.init(&m_alloc, std::span(tonemapper_slang));
  }
  

  void onDetach() override
  {
    NVVK_CHECK(vkDeviceWaitIdle(m_app->getDevice()));
    // NVVK_CHECK(vkQueueWaitIdle(m_app->getQueue(0).queue));

    ImGui_ImplVulkan_RemoveTexture(m_imguiImage);
    m_alloc.destroyImage(m_viewportImage);
    m_profilerGpuTimer.deinit();
    m_info.profilerManager->destroyTimeline(m_profilerTimeline);

    VkDevice device = m_app->getDevice();

    m_descPack.deinit();
    vkDestroyPipelineLayout(device, m_graphicPipelineLayout, nullptr);
    vkDestroyShaderEXT(device, m_vertexShader, nullptr);
    vkDestroyShaderEXT(device, m_fragmentShader, nullptr);

    m_alloc.destroyBuffer(m_sceneResource.bSceneInfo);
    m_alloc.destroyBuffer(m_sceneResource.bMeshes);
    m_alloc.destroyBuffer(m_sceneResource.bMaterials);
    m_alloc.destroyBuffer(m_sceneResource.bInstances);
    for(auto& gltfData : m_sceneResource.bGltfDatas)
    {
      m_alloc.destroyBuffer(gltfData);
    }
    for(auto& texture : m_textures)
    {
      m_alloc.destroyImage(texture);
    }

    m_gBuffers.deinit();
    m_stagingUploader.deinit();
    m_skySimple.deinit();
    // m_tonemapper.deinit();
    m_samplerPool.deinit();
    m_alloc.deinit();
  }

  void onUIRender() override
  {
    ImGui::Begin("Settings");
    ImGui::Checkbox("Animated Viewport", &m_animate);
    ImGui::TextDisabled("%d FPS / %.3fms", static_cast<int>(ImGui::GetIO().Framerate), 1000.F / ImGui::GetIO().Framerate);

    // Add window information
    const VkExtent2D& viewportSize = m_app->getViewportSize();
    ImGui::Text("Viewport Size: %d x %d", viewportSize.width, viewportSize.height);

    ImGui::End();

    // Rendered image displayed fully in 'Viewport' window
    ImGui::Begin("Viewport");
    ImGui::Image((ImTextureID)m_imguiImage, ImGui::GetContentRegionAvail());
    ImGui::End();
  }

  void onPreRender() override { m_profilerTimeline->frameAdvance(); }

  void onRender(VkCommandBuffer cmd)
  {
    if(m_animate)
    {
      auto timerSection = m_profilerGpuTimer.cmdFrameSection(cmd, "Animation");

      VkClearColorValue clearColor{};
      ImGui::ColorConvertHSVtoRGB((float)ImGui::GetTime() * 0.05f, 1, 1, clearColor.float32[0], clearColor.float32[1],
                                  clearColor.float32[2]);
      VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      nvvk::cmdImageMemoryBarrier(cmd, {m_viewportImage.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL});
      vkCmdClearColorImage(cmd, m_viewportImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);
      nvvk::cmdImageMemoryBarrier(cmd, {m_viewportImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
  }

  // Called if showMenu is true
  void onUIMenu() override
  {
    bool vsync = m_app->isVsync();

    if(ImGui::BeginMenu("File"))
    {
      if(ImGui::MenuItem("Exit", "Ctrl+Q"))
        m_app->close();
      ImGui::EndMenu();
    }
    if(ImGui::BeginMenu("View"))
    {
      ImGui::MenuItem("V-Sync", "Ctrl+Shift+V", &vsync);
      ImGui::EndMenu();
    }

    if(ImGui::IsKeyPressed(ImGuiKey_Q) && ImGui::IsKeyDown(ImGuiKey_LeftCtrl))
    {
      m_app->close();
    }

    if(ImGui::IsKeyPressed(ImGuiKey_V) && ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyDown(ImGuiKey_LeftShift))
    {
      vsync = !vsync;
    }

    if(vsync != m_app->isVsync())
    {
      m_app->setVsync(vsync);
    }
  }

  //---------------------------------------------------------------------------------------------------------------
  // Additional helper functions
  void createScene()
  {
    // create scene
    // load in teapot, plane and an image
    // create instances for them, assign materials and transformations

    VkCommandBuffer   cmd       = m_app->createTempCmdBuffer();
    // Load gltf resources
    // ensure they are in a file called "resources", in the same dir as main.cpp
    {
      tinygltf::Model teapotModel =
          nvsamples::loadGltfResources(nvutils::findFile("teapot.gltf", nvsamples::getResourcesDirs()));  // Load the GLTF resources from the file
      tinygltf::Model planeModel =
          nvsamples::loadGltfResources(nvutils::findFile("plane.gltf", nvsamples::getResourcesDirs()));  // Load the GLTF resources from the file

      //Textures
      {
        std::filesystem::path imageFilename = nvutils::findFile("tiled_floor.png", nvsamples::getResourcesDirs());
        // m_staginguploader controls what gets uploaded to the GPU
        nvvk::Image texture = nvsamples::loadAndCreateImage(cmd, m_stagingUploader, m_app->getDevice(), imageFilename);  // Load the image from the file and create a texture from it
        NVVK_DBG_NAME(texture.image);
        m_samplerPool.acquireSampler(texture.descriptor.sampler); // this sets texture.descriptor.sampler to default bilinear sampling from m_samplerPool
        m_textures.emplace_back(texture);  // Store the texture in the vector of textures, could also use push_back here, either way it adds it to m_textures
      }

      // Upload the GLTF resources to the GPU
      {
        nvsamples::importGltfData(m_sceneResource, teapotModel, m_stagingUploader);  // Import the GLTF resources, converts to GPU friendly structs
        nvsamples::importGltfData(m_sceneResource, planeModel, m_stagingUploader);   // Import the GLTF resources
      }
    }

    m_sceneResource.materials = {
        // Teapot material
        {.baseColorFactor = glm::vec4(0.8f, 1.0f, 0.6f, 1.0f), .metallicFactor = 0.5f, .roughnessFactor = 0.5f},
        // Plane material with texture
        {.baseColorFactor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), .metallicFactor = 0.1f, .roughnessFactor = 0.8f, .baseColorTextureIndex = -1}};


    m_sceneResource.instances = {
        // Teapot
        {.transform     = glm::translate(glm::mat4(1), glm::vec3(0, 0, 0)) * glm::scale(glm::mat4(1), glm::vec3(0.5f)),
         .materialIndex = 0,
         .meshIndex     = 0},
        // Plane
        {.transform = glm::scale(glm::translate(glm::mat4(1), glm::vec3(0, -0.9f, 0)), glm::vec3(2.f)), .materialIndex = 1, .meshIndex = 1},
    };

    nvsamples::createGltfSceneInfoBuffer(m_sceneResource, m_stagingUploader);  // Create uniform/ssbo buffers for the scene data (GPU buffers)

    m_stagingUploader.cmdUploadAppended(cmd);  // Upload the scene information to the GPU

    // Scene information
    shaderio::GltfSceneInfo& sceneInfo = m_sceneResource.sceneInfo;
    sceneInfo.useSky                   = false;                                         // Use light
    sceneInfo.instances = (shaderio::GltfInstance*)m_sceneResource.bInstances.address;  // Address of the instance buffer
    sceneInfo.meshes = (shaderio::GltfMesh*)m_sceneResource.bMeshes.address;            // Address of the mesh buffer
    sceneInfo.materials = (shaderio::GltfMetallicRoughness*)m_sceneResource.bMaterials.address;  // Address of the material buffer
    sceneInfo.backgroundColor             = {0.85f, 0.85f, 0.85f};                               // The background color
    sceneInfo.numLights                   = 1;
    sceneInfo.punctualLights[0].color     = glm::vec3(1.0f, 1.0f, 1.0f); // 1 point light
    sceneInfo.punctualLights[0].intensity = 4.0f;
    sceneInfo.punctualLights[0].position  = glm::vec3(1.0f, 1.0f, 1.0f);  // Position of the light
    sceneInfo.punctualLights[0].direction = glm::vec3(1.0f, 1.0f, 1.0f);  // Direction to the light
    sceneInfo.punctualLights[0].type      = shaderio::GltfLightType::ePoint;
    sceneInfo.punctualLights[0].coneAngle = 0.9f;  // Cone angle for spot lights (0 for point and directional lights)

    m_app->submitAndWaitTempCmdBuffer(cmd);
    // m_stagingUploader.releaseStaging();

    // Default camera
    m_cameraManip->setClipPlanes({0.01F, 100.0F});
    m_cameraManip->setLookat({0.0F, 0.5F, 5.0}, {0.F, 0.F, 0.F}, {0.0F, 1.0F, 0.0F});

    LOGI(" CREATE SCENE RAN \n");
  }

  //---------------------------------------------------------------------------------------------------------------
  // The Vulkan descriptor set defines the resources that are used by the shaders (textures, buffers etc).
  // Here we add the bindings for the textures.
  void createGraphicsDescriptorSetLayout()
  {
    nvvk::DescriptorBindings bindings;
    bindings.addBinding({.binding         = shaderio::BindingPoints::eTextures,
                         .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         .descriptorCount = 10,  // Maximum number of textures used in the scene
                         .stageFlags      = VK_SHADER_STAGE_ALL}, // accessible from every stage, vertex fragment, compute etc
                         // Binding flags: :make the texture array flexible
                         // 1. Descriptors in binding can be updated after bound
                         // 2. Descriptors which aren't being used by shader can be updated (while cmd is still in flight)
                         // 3. not all 10 slots need to be filled
                        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
                            | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);

    // Creating the descriptor set and set layout from the bindings
    // this is then used for createGraphicsPipelineLayout
    // m_descPack is from nvvk::DescriptorPack 
    m_descPack.init(bindings, m_app->getDevice(), 1, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                    VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);

    // attach human-readable debug names
    NVVK_DBG_NAME(m_descPack.getLayout());
    NVVK_DBG_NAME(m_descPack.getPool());
    NVVK_DBG_NAME(m_descPack.getSet(0));

    LOGI(" createGraphicsDescriptorSetLayout RAN \n");
  }


  //--------------------------------------------------------------------------------------------------
  // The graphic pipeline is all the stages that are used to render a section of the scene.
  // Stages like: vertex shader, fragment shader, rasterization, and blending.
  //
  void createGraphicsPipelineLayout()
  {
    // Push constant: used to pass data to the shader at each frame
    // here is just declaring how much space to reserve for the push constants
    // push constants are used here as they are cheaper (thank UBO etc) to pass small, frequently changing per draw call data
    // no descriptor set or buffer write needed
    const VkPushConstantRange pushConstantRange{
        .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
        .offset = 0, 
        .size = sizeof(shaderio::TutoPushConstant)};

    // The pipeline layout is used to pass data to the pipeline, anything with "layout" in the shader
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = m_descPack.getLayoutPtr(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pushConstantRange,
    };
    NVVK_CHECK(vkCreatePipelineLayout(m_app->getDevice(), &pipelineLayoutInfo, nullptr, &m_graphicPipelineLayout));
    NVVK_DBG_NAME(m_graphicPipelineLayout);

    LOGI(" createGraphicsPipelineLayout RAN \n");
  }

  //--------------------------------------------------------------------------------------------------
  // Update the textures: this is called when the scene is loaded
  // Textures are updated in the descriptor set (0)
  void updateTextures()
  {
    if(m_textures.empty())
      return;

    // Update the descriptor set with the textures
    // writesetcontainer: a convience wrapper that holds backing storage, since writdescriptorset does not hold actual data (only pointers)
    nvvk::WriteSetContainer write{};

    VkWriteDescriptorSet    allTextures =
        m_descPack.makeWrite(shaderio::BindingPoints::eTextures, 0, 0, uint32_t(m_textures.size()));
        
    nvvk::Image* allImages = m_textures.data();

    write.append(allTextures, allImages);
    vkUpdateDescriptorSets(m_app->getDevice(), write.size(), write.data(), 0, nullptr);
  }


  // -------------------------------------------------------------------------------------------------
  VkShaderModuleCreateInfo compileSlangShader(const std::filesystem::path& filename, const std::span<const uint32_t>& spirv)
  {
    // use pre-compiled spirv by default
    VkShaderModuleCreateInfo shaderCode = nvsamples::getShaderModuleCreateInfo(spirv);

    // Try to hot compile the shader
    // 1. try to find shader folder
    std::filesystem::path shaderSource = nvutils::findFile(filename, nvsamples::getShaderDirs());
    if(m_slangCompiler.compileFile(shaderSource))
    {
      // use nvidia's slang compiler nvslang to compile the shaders on the fly
      shaderCode.codeSize = m_slangCompiler.getSpirvSize();
      shaderCode.pCode    = m_slangCompiler.getSpirv();
    }
    else
    {
      LOGE("Error compiling shaders: %s\n%s\n", shaderSource.string().c_str(),
           m_slangCompiler.getLastDiagnosticMessage().c_str());
    }
    return shaderCode;
  }

  //---------------------------------------------------------------------------------------------------------------
  // Compile the graphics shaders and create the shader modules.
  // This function only creates vertex and fragment shader modules for the graphics pipeline.
  // The actual graphics pipeline is created elsewhere and uses these shader modules.
  // This function will use the pre-compiled shaders if the compilation fails.
  void compileAndCreateGraphicsShaders()
  {
    // the compilation is handled in CMakeLists.txt, spirv is generated during build
    VkShaderModuleCreateInfo shaderCode = compileSlangShader("foundation.slang", foundation_slang);
    // Destroy previous shaders (if they exist)
    vkDestroyShaderEXT(m_app->getDevice(), m_vertexShader, nullptr);
    vkDestroyShaderEXT(m_app->getDevice(), m_fragmentShader, nullptr);

    //Push constants
    const VkPushConstantRange pushConstantRange{
      .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
      .offset     = 0,
      .size       = sizeof(shaderio::TutoPushConstant),
    };

    // used to create shader modules, shared handles between vertex and fragment
    VkShaderCreateInfoEXT shaderInfo{
      .sType                  = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
      .codeType               = VK_SHADER_CODE_TYPE_SPIRV_EXT,
      .codeSize               = shaderInfo.codeSize,
      .pCode                  = shaderCode.pCode,
      .setLayoutCount         = 1,
      .pSetLayouts            = m_descPack.getLayoutPtr(),
      .pushConstantRangeCount = 1,
      .pPushConstantRanges    = &pushConstantRange,
    };

    // vertex shader
    shaderInfo.stage    = VK_SHADER_STAGE_VERTEX_BIT;
    shaderInfo.nextStage= VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderInfo.pName    = "vertexMain"; // this is the name of the vertex shader function in foundation.slang
    vkCreateShadersEXT(m_app->getDevice(), 1U, &shaderInfo, nullptr, &m_vertexShader);
    NVVK_DBG_NAME(m_vertexShader);

    // fragment shader
    shaderInfo.stage    = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderInfo.nextStage= 0;
    shaderInfo.pName    = "fragmentMain";
    vkCreateShadersEXT(m_app->getDevice(), 1U, &shaderInfo, nullptr, &m_fragmentShader);
    NVVK_DBG_NAME(m_fragmentShader);
  }

  // Accessor for camera manipulator
  std::shared_ptr<nvutils::CameraManipulator> getCameraManipulator() const { return m_cameraManip; }

private:
  Info m_info;
  bool m_animate = false;

  nvvk::ResourceAllocator m_alloc{};
  nvapp::Application*     m_app{};
  nvvk::SamplerPool       m_samplerPool{};
  nvvk::StagingUploader   m_stagingUploader{};

  // Rasterization
  nvvk::GBuffer           m_gBuffers{};         // The G-Buffer
  nvslang::SlangCompiler  m_slangCompiler{};    // The Slang compiler used to compile the shaders
  // Rasterization: Camera manipulator
  std::shared_ptr<nvutils::CameraManipulator> m_cameraManip{std::make_shared<nvutils::CameraManipulator>()};
  // Pipeline
  nvvk::DescriptorPack m_descPack;  // The descriptor bindings used to create the descriptor set layout and descriptor sets (bind textures, buffers etc)
  VkPipelineLayout m_graphicPipelineLayout{};  // The pipeline layout for the graphics pipeline
  //shaders
  VkShaderEXT m_vertexShader{};
  VkShaderEXT m_fragmentShader{}; 

  nvutils::ProfilerTimeline* m_profilerTimeline{};
  nvvk::ProfilerGpuTimer     m_profilerGpuTimer;

  // Rasterization: Scene information buffer (UBO)
  nvsamples::GltfSceneResource m_sceneResource{};  // The GLTF scene resource, contains all the buffers and data for the scene
  std::vector<nvvk::Image> m_textures{};           // Textures used in the scene
  nvshaders::SkySimple     m_skySimple{};       // Sky rendering
  // nvshaders::Tonemapper    m_tonemapper{};      // Tonemapper for post-processing effects
  // shaderio::TonemapperData m_tonemapperData{};  // Tonemapper data used to pass parameters to the tonemapper shader
  glm::vec2 m_metallicRoughnessOverride{-0.01f, -0.01f};  // Override values for metallic and roughness, used in the UI to control the material properties

  nvvk::Image m_viewportImage{};

  VkDescriptorSet m_imguiImage{};
};


int main(int argc, char** argv)
{
  nvutils::ProfilerManager   profilerManager;
  nvutils::ParameterRegistry parameterRegistry;
  nvutils::ParameterParser   parameterParser;

  // setup sample element
  SampleElement::Info sampleInfo = {
      .profilerManager   = &profilerManager,
      .parameterRegistry = &parameterRegistry,
  };
  std::shared_ptr<SampleElement> sampleElement = std::make_shared<SampleElement>(sampleInfo);

  // setup logger element, `true` means shown by default
  // we add it early so outputs are captured early on, you might want to defer this to a later timer.
  std::shared_ptr<nvapp::ElementLogger> elementLogger = std::make_shared<nvapp::ElementLogger>(true);
  nvutils::Logger::getInstance().setLogCallback([&](nvutils::Logger::LogLevel logLevel, const std::string& text) {
    elementLogger->addLog(logLevel, "%s", text.c_str());
  });

  nvvk::ContextInitInfo vkSetup{
      .instanceExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME},
      .deviceExtensions   = {{VK_KHR_SWAPCHAIN_EXTENSION_NAME}},
  };

  // let's add a command-line option to enable/disable validation layers
  parameterRegistry.add({"validation"}, &vkSetup.enableValidationLayers);
  parameterRegistry.add({"verbose"}, &vkSetup.verbose);
  // as well as an option to force the vulkan device based on canonical index
  parameterRegistry.add({"forcedevice"}, &vkSetup.forceGPU);

  // add all parameters to the parser
  parameterParser.add(parameterRegistry);

  // and then parse command line
  parameterParser.parse(argc, argv);

  nvvk::addSurfaceExtensions(vkSetup.instanceExtensions);
  nvvk::Context vkContext;
  if(vkContext.init(vkSetup) != VK_SUCCESS)
  {
    LOGE("Error in Vulkan context creation\n");
    return 1;
  }

  nvapp::ApplicationCreateInfo appInfo;
  appInfo.name           = "The Empty Example";
  appInfo.useMenu        = true;
  appInfo.instance       = vkContext.getInstance();
  appInfo.device         = vkContext.getDevice();
  appInfo.physicalDevice = vkContext.getPhysicalDevice();
  appInfo.queues         = vkContext.getQueueInfos();
  appInfo.dockSetup      = [](ImGuiID viewportID) {
    // right side panel container
    ImGuiID settingID = ImGui::DockBuilderSplitNode(viewportID, ImGuiDir_Right, 0.25F, nullptr, &viewportID);
    ImGui::DockBuilderDockWindow("Settings", settingID);

    // bottom panel container
    ImGuiID loggerID = ImGui::DockBuilderSplitNode(viewportID, ImGuiDir_Down, 0.35F, nullptr, &viewportID);
    ImGui::DockBuilderDockWindow("Log", loggerID);
    ImGuiID profilerID = ImGui::DockBuilderSplitNode(loggerID, ImGuiDir_Right, 0.4F, nullptr, &loggerID);
    ImGui::DockBuilderDockWindow("Profiler", profilerID);
  };

  // Create the application
  nvapp::Application app;
  app.init(appInfo);

  // add the sample main element
  app.addElement(sampleElement);
  app.addElement(std::make_shared<nvapp::ElementDefaultWindowTitle>());
  // add profiler element
  app.addElement(std::make_shared<nvapp::ElementProfiler>(&profilerManager));
  // add logger element
  app.addElement(elementLogger);

  LOGI("%s", "Wohoo let's run this sample!\n");

  // enter the main loop
  app.run();

  // Cleanup in reverse order
  app.deinit();
  vkContext.deinit();

  return 0;
}