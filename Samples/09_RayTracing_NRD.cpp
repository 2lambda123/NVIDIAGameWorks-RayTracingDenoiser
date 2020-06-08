/*
Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "SampleBase.h"
#include "Extensions/NRIRayTracing.h"
#include "NRDIntegration.h"

#include <array>
#include <io.h>

constexpr auto BUILD_FLAGS = nri::AccelerationStructureBuildBits::PREFER_FAST_TRACE;
constexpr uint32_t TEXTURES_PER_MATERIAL = 4;
constexpr uint32_t FG_TEX_SIZE = 256;
constexpr float NEAR_Z = 0.01f; // m
constexpr bool CAMERA_RELATIVE = true;
constexpr bool CAMERA_LEFT_HANDED = true;
constexpr uint32_t ANIMATED_INSTANCE_MAX_NUM = 512;

// See HLSL
#define FLAG_FIRST_BIT                  20
#define INSTANCE_ID_MASK                ( ( 1 << FLAG_FIRST_BIT ) - 1 )
#define FLAG_OPAQUE_OR_ALPHA_OPAQUE     0x01
#define FLAG_TRANSPARENT                0x02
#define FLAG_EMISSION                   0x04
#define FLAG_FORCED_EMISSION            0x08

enum ShaderGroup : uint32_t
{
    Raytracing00_rgen,
    Raytracing01_rgen,
    Raytracing10_rgen,
    Raytracing11_rgen,
    Main_rmiss,
    Main_rhit
};

enum class Buffer : uint32_t
{
    GlobalConstants,
    TlasDataStaging,
    InstanceDataStaging,

    ShaderTable,
    PrimitiveData,
    InstanceData,
    Scratch,

    UploadHeapBufferNum = 3
};

enum class Texture : uint32_t
{
    IntegrateBRDF,
    ViewZ,
    DirectLighting,
    TransparentLighting,
    ObjectMotion,
    Normal_Roughness,
    BaseColor_Metalness,
    Shadow,
    DiffHit,
    SpecHit,
    Unfiltered_Shadow,
    Unfiltered_DiffA,
    Unfiltered_DiffB,
    Unfiltered_SpecHit,
    Composition,
    CompositionHdr,
    TaaHistory,
    TaaHistoryPrev,
    Final,
    MaterialTextures
};

enum class Pipeline : uint32_t
{
    IntegrateBRDF,
    Raytracing,
    Composition,
    Temporal,
};

enum class Descriptor : uint32_t
{
    Opaque_AccelerationStructure,

    PrimitiveData_Buffer,
    InstanceData_Buffer,

    IntegrateBRDF_Texture,
    IntegrateBRDF_StorageTexture,
    ViewZ_Texture,
    ViewZ_StorageTexture,
    DirectLighting_Texture,
    DirectLighting_StorageTexture,
    TransparentLighting_Texture,
    TransparentLighting_StorageTexture,
    ObjectMotion_Texture,
    ObjectMotion_StorageTexture,
    Normal_Roughness_Texture,
    Normal_Roughness_StorageTexture,
    BaseColor_Metalness_Texture,
    BaseColor_Metalness_StorageTexture,
    Shadow_Texture,
    Shadow_StorageTexture,
    DiffHit_Texture,
    DiffHit_StorageTexture,
    SpecHit_Texture,
    SpecHit_StorageTexture,
    Unfiltered_Shadow_Texture,
    Unfiltered_Shadow_StorageTexture,
    Unfiltered_DiffA_Texture,
    Unfiltered_DiffA_StorageTexture,
    Unfiltered_DiffB_Texture,
    Unfiltered_DiffB_StorageTexture,
    Unfiltered_SpecHit_Texture,
    Unfiltered_SpecHit_StorageTexture,
    Composition_Texture,
    Composition_StorageTexture,
    CompositionHdr_Texture,
    CompositionHdr_StorageTexture,
    TaaHistory_Texture,
    TaaHistory_StorageTexture,
    TaaHistoryPrev_Texture,
    TaaHistoryPrev_StorageTexture,
    Final_Texture,
    Final_StorageTexture,
    MaterialTextures,
};

enum class DescriptorSet : uint32_t
{
    IntegrateBRDF0,
    Raytracing2,
    Raytracing1,
    Composition1,
    Temporal1a,
    Temporal1b,
};

struct NRIInterface
    : public nri::CoreInterface
    , public nri::SwapChainInterface
    , public nri::RayTracingInterface
{};

struct Frame
{
    nri::DeviceSemaphore* deviceSemaphore;
    nri::CommandAllocator* commandAllocator;
    std::array<nri::CommandBuffer*, 3> commandBuffers;
    nri::Descriptor* globalConstantBufferDescriptor;
    nri::DescriptorSet* globalConstantBufferDescriptorSet;
    uint64_t globalConstantBufferOffset;
};

struct GlobalConstantBufferData
{
    float4x4 gWorldToView;
    float4x4 gViewToWorld;
    float4x4 gViewToClip;
    float4x4 gWorldToClipPrev;
    float4x4 gWorldToClip;
    float4 gCameraFrustum;
    float4 gSunDirection_gExposure;
    float4 gWorldOrigin_gTaa;
    float4 gTrimmingParams_gEmissionIntensity;
    float2 gScreenSize;
    float2 gInvScreenSize;
    float2 gJitter;
    float gAmbient;
    float gSeparator;
    float gNearZ;
    float gRoughnessOverride;
    float gMetalnessOverride;
    float gDiffHitDistScale;
    float gSpecHitDistScale;
    float gUnitsToMetersMultiplier;
    float gIndirectDiffuse;
    float gIndirectSpecular;
    float gTanSunAngularDiameter;
    float gPixelAngularDiameter;
    float gSunAngularDiameter;
    float gUseMipmapping;
    float gIsOrtho;
    float gDebug;
    float gDiffSecondBounce;
    float gTransparent;
    uint32_t gOnScreen;
    uint32_t gFrameIndex;
    uint32_t gForcedMaterial;
    uint32_t gPrimaryFullBrdf;
    uint32_t gIndirectFullBrdf;
    uint32_t gUseNormalMap;
    uint32_t gWorldSpaceMotion;
    uint32_t gUseBlueNoise;
    uint32_t gDither;
};

struct Settings
{
    uint32_t version = 1;

    double motionStartTime = 0.0;

    float camFov = 90.0f;
    float diffDenoisingRadius = 30.0f;
    float diffAdaptiveRadiusScale = 5.0f;
    float diffHitDistScale = 3.0f;
    float specDenoisingRadius = 40.0f;
    float specAdaptiveRadiusScale = 0.5f;
    float specHitDistScale = 3.0f;
    float antilagIntensityThreshold = 1.0f;
    float sunAzimuth = -147.0f;
    float sunElevation = 45.0f;
    float sunAngularDiameter = 0.533f;
    float exposure = 0.00017f;
    float roughnessOverride = 0.0f;
    float metalnessOverride = 0.0f;
    float emissionIntensity = 2000.0f;
    float skyAmbient = 0.0f;
    float disocclusionThreshold = 0.5f;
    float debug = 0.0f;
    float unitsToMetersMultiplier = 1.0f;
    float emulateMotionSpeed = 1.0f;
    float animatedObjectScale = 1.0f;
    float separator = 0.0f;
    float animationProgress = 0.0f;
    float animationSpeed = 0.0f;
    float sharpness = 0.01f;

    int32_t onScreen = 0;
    int32_t diffMaxHistoryFrameNum = 31;
    int32_t specMaxHistoryFrameNum = 31;
    int32_t forcedMaterial;
    int32_t animatedObjectNum = 5;
    int32_t activeAnimation = 0;
    int32_t motionMode = 0;

    bool primaryFullBrdf = true;
    bool indirectFullBrdf = true;
    bool indirectDiffuse = true;
    bool indirectSpecular = true;
    bool normalMap = true;
    bool mip = true;
    bool blueNoise = true;
    bool metalAmbient = true;
    bool temporal = true;
    bool reference = false;
    bool syncHistoryFrames = false;
    bool checkerboard = true;
    bool specSecondBounce = false;
    bool diffSecondBounce = true;
    bool animatedObjects = false;
    bool animateCamera = false;
    bool animateSun = false;
    bool nineBrothers = false;
    bool blink = false;
    bool pauseAnimation = true;
    bool emission = false;
    bool specularAnisotropicFiltering = true;
    bool worldSpaceMotion = true;
    bool linear = true;
    bool emissiveObjects = false;
    bool antilag = true;
};

struct DescriptorDesc
{
    const char* debugName;
    void* resource;
    nri::Format format;
    nri::TextureUsageBits textureUsage;
    nri::BufferUsageBits bufferUsage;
    bool isArray;
};

struct TextureState
{
    Texture texture;
    nri::AccessBits nextAccess;
    nri::TextureLayout nextLayout;
};

struct AnimationParameters
{
    float3 rotationAxis;
    float3 elipseAxis;
    float durationSec = 5.0f;
    float progressedSec = 0.0f;
    float inverseRotation = 1.0f;
    float inverseDirection = 1.0f;
    float angleRad = 0.0f;
};

struct AnimatedInstance
{
    double3 position = double3::Zero();
    double3 basePosition = double3::Zero();
    AnimationParameters animation;
    uint32_t instanceID;

    float4x4 Animate(float elapsedSeconds, float scale)
    {
        float weight = (animation.progressedSec + elapsedSeconds) / animation.durationSec;
        weight = weight * 2.0f - 1.0f;
        weight = Pi(weight);

        float3 localPosition;
        localPosition.x = Cos(weight * animation.inverseDirection);
        localPosition.y = Sin(weight * animation.inverseDirection);
        localPosition.z = localPosition.y;

        position = basePosition + ToDouble( localPosition * animation.elipseAxis * scale );

        animation.angleRad = weight * animation.inverseRotation;
        animation.progressedSec += elapsedSeconds;
        animation.progressedSec = (animation.progressedSec >= animation.durationSec) ? 0.0f : animation.progressedSec;

        float4x4 transform;
        transform.SetupByRotation(animation.angleRad, animation.rotationAxis);
        transform.AddScale(scale);

        return transform;
    }
};

struct PrimitiveData
{
    uint32_t uv0;
    uint32_t uv1;
    uint32_t uv2;
    uint32_t fnX_fnY;

    uint32_t fnZ_worldToUvUnits;
    uint32_t n0X_n0Y;
    uint32_t n0Z_n1X;
    uint32_t n1Y_n1Z;

    uint32_t n2X_n2Y;
    uint32_t n2Z_t0X;
    uint32_t t0Y_t0Z;
    uint32_t t1X_t1Y;

    uint32_t t1Z_t2X;
    uint32_t t2Y_t2Z;
    uint32_t b0S_b1S;
    uint32_t b2S;
};

struct InstanceData
{
    float4 mObjectToWorld0_basePrimitiveId;
    float4 mObjectToWorld1_baseTextureIndex;
    float4 mObjectToWorld2_averageBaseColor;

    float4 mWorldToWorldPrev0;
    float4 mWorldToWorldPrev1;
    float4 mWorldToWorldPrev2;
};

class Sample : public SampleBase
{
public:
    Sample()
    {}

    ~Sample();

    bool Initialize(nri::GraphicsAPI graphicsAPI);
    void PrepareFrame(uint32_t frameIndex);
    void RenderFrame(uint32_t frameIndex);

    inline nri::Texture*& Get(Texture index)
    { return m_Textures[(uint32_t)index]; }

    inline nri::TextureTransitionBarrierDesc& GetState(Texture index)
    { return m_TextureStates[(uint32_t)index]; }

    inline nri::Buffer*& Get(Buffer index)
    { return m_Buffers[(uint32_t)index]; }

    inline nri::Pipeline*& Get(Pipeline index)
    { return m_Pipelines[(uint32_t)index]; }

    inline nri::PipelineLayout*& GetPipelineLayout(Pipeline index)
    { return m_PipelineLayouts[(uint32_t)index]; }

    inline nri::Descriptor*& Get(Descriptor index)
    { return m_Descriptors[(uint32_t)index]; }

    inline nri::DescriptorSet*& Get(DescriptorSet index)
    { return m_DescriptorSets[(uint32_t)index]; }

private:
    void CreateCommandBuffers();
    void CreateSwapChain(nri::Format& swapChainFormat);
    void CreateResources(nri::Format swapChainFormat);
    void CreatePipelines();
    void CreateDescriptorSets();
    void CreateBottomLevelAccelerationStructures();
    void CreateTopLevelAccelerationStructure();
    void UpdateShaderTable();
    void UpdateConstantBuffer(uint32_t frameIndex);
    void UploadStaticData();
    void LoadScene();
    void SetupAnimatedObjects();
    void CreateUploadBuffer(uint64_t size, nri::Buffer*& buffer, nri::Memory*& memory);
    void CreateScratchBuffer(nri::AccelerationStructure& accelerationStructure, nri::Buffer*& buffer, nri::Memory*& memory);
    void BuildBottomLevelAccelerationStructure(nri::AccelerationStructure& accelerationStructure, const nri::GeometryObject* objects, const uint32_t objectNum);
    void BuildTopLevelAccelerationStructure(nri::CommandBuffer& commandBuffer, uint32_t bufferedFrameIndex);
    void CreateTexture(std::vector<DescriptorDesc>& descriptorDescs, const char* debugName, nri::Format format, uint32_t width, uint32_t height, uint32_t mipNum, uint32_t arraySize, nri::TextureUsageBits usage, nri::AccessBits state);
    void CreateBuffer(std::vector<DescriptorDesc>& descriptorDescs, const char* debugName, uint64_t elements, uint32_t stride, nri::BufferUsageBits usage, nri::Format format = nri::Format::UNKNOWN);
    void CreateDescriptors(const std::vector<DescriptorDesc>& descriptorDescs);
    uint32_t BuildOptimizedTransitions(const TextureState* states, uint32_t stateNum, nri::TextureTransitionBarrierDesc* transitions, uint32_t transitionMaxNum);

    inline float3 GetSunDirection() const
    {
        float3 sunDirection;
        sunDirection.x = Cos( DegToRad(m_Settings.sunAzimuth) ) * Cos( DegToRad(m_Settings.sunElevation) );
        sunDirection.y = Sin( DegToRad(m_Settings.sunAzimuth) ) * Cos( DegToRad(m_Settings.sunElevation) );
        sunDirection.z = Sin( DegToRad(m_Settings.sunElevation) );

        return sunDirection;
    }

    inline float3 GetTrimmingParams() const
    {
        if (m_Settings.reference)
            return float3(1.0f, 0.0f, 0.0001f);

        float3 params;
        params.x = 0.85f;
        params.y = 0.04f;
        params.z = 0.11f;

        return params;
    }

private:
    Nrd m_NRD;

    NRIInterface NRI = {};
    nri::Device* m_Device = nullptr;
    nri::SwapChain* m_SwapChain = nullptr;
    nri::CommandQueue* m_CommandQueue = nullptr;
    nri::QueueSemaphore* m_BackBufferAcquireSemaphore = nullptr;
    nri::QueueSemaphore* m_BackBufferReleaseSemaphore = nullptr;
    nri::AccelerationStructure* m_TLAS = nullptr;
    nri::DescriptorPool* m_DescriptorPool = nullptr;
    std::array<Frame, BUFFERED_FRAME_MAX_NUM> m_Frames;
    std::vector<nri::Texture*> m_Textures;
    std::vector<nri::TextureTransitionBarrierDesc> m_TextureStates;
    std::vector<nri::Buffer*> m_Buffers;
    std::vector<nri::Memory*> m_Memories;
    std::vector<nri::Descriptor*> m_Descriptors;
    std::vector<nri::DescriptorSet*> m_DescriptorSets;
    std::vector<nri::PipelineLayout*> m_PipelineLayouts;
    std::vector<nri::Pipeline*> m_Pipelines;
    std::vector<nri::AccelerationStructure*> m_BLASs;
    std::vector<uint64_t> m_ShaderEntries;
    std::vector<BackBuffer> m_SwapChainBuffers;

    std::vector<AnimatedInstance> m_AnimatedInstances;
    Timer m_Timer;
    float m_FrameTimes[256] = {};
    float3 m_PrevLocalPos = {};
    uint2 m_OutputResolution = {};
    uint2 m_RenderResolution = {};
    utils::Scene m_Scene;
    Settings m_Settings = {};
    Settings m_PrevSettings = {};
    const nri::DeviceDesc* m_DeviceDesc = nullptr;
    uint64_t m_ConstantBufferSize = 0;
    uint32_t m_DefaultInstancesOffset = 0;
    int32_t m_TestingLocation = 0;
    bool m_PrevIsActive = true;
    bool m_HasTransparentObjects = false;
};

Sample::~Sample()
{
    helper::WaitIdle(NRI, *m_Device, *m_CommandQueue);

    m_NRD.Destroy();

    for (Frame& frame : m_Frames)
    {
        for (nri::CommandBuffer*& commandBuffer : frame.commandBuffers)
            NRI.DestroyCommandBuffer(*commandBuffer);
        NRI.DestroyDeviceSemaphore(*frame.deviceSemaphore);
        NRI.DestroyCommandAllocator(*frame.commandAllocator);
        NRI.DestroyDescriptor(*frame.globalConstantBufferDescriptor);
    }

    for (BackBuffer& backBuffer : m_SwapChainBuffers)
    {
        NRI.DestroyDescriptor(*backBuffer.colorAttachment);
        NRI.DestroyFrameBuffer(*backBuffer.frameBufferUI);
    }

    for (uint32_t i = 0; i < m_Textures.size(); i++)
        NRI.DestroyTexture(*m_Textures[i]);

    for (uint32_t i = 0; i < m_Buffers.size(); i++)
        NRI.DestroyBuffer(*m_Buffers[i]);

    for (uint32_t i = 0; i < m_Descriptors.size(); i++)
        NRI.DestroyDescriptor(*m_Descriptors[i]);

    for (uint32_t i = 0; i < m_Pipelines.size(); i++)
        NRI.DestroyPipeline(*m_Pipelines[i]);

    for (uint32_t i = 0; i < m_PipelineLayouts.size(); i++)
        NRI.DestroyPipelineLayout(*m_PipelineLayouts[i]);

    for (uint32_t i = 0; i < m_BLASs.size(); i++)
        NRI.DestroyAccelerationStructure(*m_BLASs[i]);

    NRI.DestroyDescriptorPool(*m_DescriptorPool);
    NRI.DestroyAccelerationStructure(*m_TLAS);
    NRI.DestroyQueueSemaphore(*m_BackBufferAcquireSemaphore);
    NRI.DestroyQueueSemaphore(*m_BackBufferReleaseSemaphore);
    NRI.DestroySwapChain(*m_SwapChain);

    for (size_t i = 0; i < m_Memories.size(); i++)
        NRI.FreeMemory(*m_Memories[i]);

    m_UserInterface.Shutdown();

    nri::DestroyDevice(*m_Device);
}

bool Sample::Initialize(nri::GraphicsAPI graphicsAPI)
{
    // Slilently change D3D11, which doesn't support ray tracing, to D3D12
    graphicsAPI = graphicsAPI == nri::GraphicsAPI::D3D11 ? nri::GraphicsAPI::D3D12 : graphicsAPI;

    nri::DeviceCreationDesc deviceCreationDesc = {};
    deviceCreationDesc.graphicsAPI = graphicsAPI;
    deviceCreationDesc.enableAPIValidation = m_DebugAPI;
    deviceCreationDesc.enableNRIValidation = m_DebugNRI;
    deviceCreationDesc.spirvBindingOffsets = SPIRV_BINDING_OFFSETS;
    NRI_ABORT_ON_FAILURE( nri::CreateDevice(deviceCreationDesc, m_Device) );

    NRI_ABORT_ON_FAILURE( nri::GetInterface(*m_Device, NRI_INTERFACE(nri::CoreInterface), (nri::CoreInterface*)&NRI) );
    NRI_ABORT_ON_FAILURE( nri::GetInterface(*m_Device, NRI_INTERFACE(nri::SwapChainInterface), (nri::SwapChainInterface*)&NRI) );
    NRI_ABORT_ON_FAILURE( nri::GetInterface(*m_Device, NRI_INTERFACE(nri::RayTracingInterface), (nri::RayTracingInterface*)&NRI) );

    NRI_ABORT_ON_FAILURE( NRI.GetCommandQueue(*m_Device, nri::CommandQueueType::GRAPHICS, m_CommandQueue));
    NRI_ABORT_ON_FAILURE( NRI.CreateQueueSemaphore(*m_Device, m_BackBufferAcquireSemaphore));
    NRI_ABORT_ON_FAILURE( NRI.CreateQueueSemaphore(*m_Device, m_BackBufferReleaseSemaphore));

    m_DeviceDesc = &NRI.GetDeviceDesc(*m_Device);
    m_ConstantBufferSize = helper::GetAlignedSize(sizeof(GlobalConstantBufferData), m_DeviceDesc->constantBufferOffsetAlignment);

    LoadScene();

    m_OutputResolution = uint2(GetWindowWidth(), GetWindowHeight());
    m_RenderResolution = m_OutputResolution;

    nri::Format swapChainFormat = nri::Format::UNKNOWN;
    CreateCommandBuffers();
    CreateSwapChain(swapChainFormat);
    CreatePipelines();
    CreateBottomLevelAccelerationStructures();
    CreateTopLevelAccelerationStructure();
    CreateResources(swapChainFormat);
    CreateDescriptorSets();
    UpdateShaderTable();
    UploadStaticData();
    SetupAnimatedObjects();

    const nrd::MethodDesc methodDescs[] =
    {
        { nrd::Method::DIFFUSE, m_RenderResolution.x, m_RenderResolution.y },
        { nrd::Method::SPECULAR, m_RenderResolution.x, m_RenderResolution.y },
        { nrd::Method::SHADOW, m_RenderResolution.x, m_RenderResolution.y },
    };

    nrd::DenoiserCreationDesc denoiserCreationDesc = {};
    denoiserCreationDesc.requestedMethods = methodDescs;
    denoiserCreationDesc.requestedMethodNum = helper::GetCountOf(methodDescs);
    NRI_ABORT_ON_FALSE( m_NRD.Initialize(*m_Device, NRI, denoiserCreationDesc, false) );

    m_Camera.Initialize(m_Scene.aabb.GetCenter(), m_Scene.aabb.vMin, CAMERA_RELATIVE, CAMERA_LEFT_HANDED);
    m_Scene.UnloadResources();

    return m_UserInterface.Initialize(m_hWnd, *m_Device, NRI, m_OutputResolution.x, m_OutputResolution.y, BUFFERED_FRAME_MAX_NUM, swapChainFormat);
}

void Sample::SetupAnimatedObjects()
{
    const float3 maxSize = Abs(m_Scene.aabb.vMax) + Abs(m_Scene.aabb.vMin);

    Rand::Seed(106937);

    for (uint32_t i = 0; i < ANIMATED_INSTANCE_MAX_NUM; i++)
    {
        uint32_t instanceIndex = i % m_DefaultInstancesOffset;
        float3 tmpPosition = Rand::uf3() * maxSize - Abs(m_Scene.aabb.vMin);

        AnimatedInstance tmpAnimatedInstance = {};
        tmpAnimatedInstance.instanceID = helper::GetCountOf(m_Scene.instances);
        tmpAnimatedInstance.position = ToDouble( tmpPosition );
        tmpAnimatedInstance.basePosition = tmpAnimatedInstance.position;
        tmpAnimatedInstance.animation.durationSec = Rand::uf1() * 10.0f + 5.0f;
        tmpAnimatedInstance.animation.progressedSec = tmpAnimatedInstance.animation.durationSec * Rand::uf1();
        tmpAnimatedInstance.animation.rotationAxis = Normalize( Rand::sf3() + 1e-6f );
        tmpAnimatedInstance.animation.elipseAxis = Rand::sf3() * 5.0f;
        tmpAnimatedInstance.animation.inverseDirection = Sign( Rand::sf1() );
        tmpAnimatedInstance.animation.inverseRotation = Sign( Rand::sf1() );
        m_AnimatedInstances.push_back(tmpAnimatedInstance);

        const utils::Instance& tmpInstance = m_Scene.instances[instanceIndex];
        m_Scene.instances.push_back(tmpInstance);
    }
}

void Sample::PrepareFrame(uint32_t frameIndex)
{
    bool isPressedReloadShaders = false;
    const float sceneRadius = m_Scene.aabb.GetRadius() * m_Settings.unitsToMetersMultiplier;

    m_PrevSettings = m_Settings;

    m_UserInterface.Prepare();

    if (!m_Input.IsKeyPressed(Key::LAlt))
    {
        ImGui::SetNextWindowPos(ImVec2(5.0f, 5.0f), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));
        ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
        {
            float avgFrameTime = m_Timer.GetSmoothedElapsedTime();
            char avg[64];
            sprintf_s(avg, "%.1f FPS (%.2f ms)", 1000.0f / avgFrameTime, avgFrameTime);

            ImVec4 colorFps = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
            if (avgFrameTime > 1000.0f / 60.0f)
                colorFps = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            if (avgFrameTime > 1000.0f / 30.0f)
                colorFps = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);

            float lo = m_Timer.GetVerySmoothedElapsedTime() * 0.5f;
            float hi = m_Timer.GetVerySmoothedElapsedTime() * 1.5f;

            const uint32_t N = helper::GetCountOf(m_FrameTimes);
            uint32_t head = frameIndex % N;
            m_FrameTimes[head] = m_Timer.GetElapsedTime();
            ImGui::PushStyleColor(ImGuiCol_Text, colorFps);
            ImGui::PlotLines("Performance", m_FrameTimes, N, head, avg, lo, hi, ImVec2(0, 80.0f));
            ImGui::PopStyleColor();

            if( m_Input.IsButtonPressed(Button::Right) )
            {
                ImGui::Text("Move - W/S/A/D");
                ImGui::Text("Accelerate - MOUSE SCROLL");
            }
            else
            {
                ImGui::PushID("CAMERA");
                {
                    static const char* onScreenModes[] =
                    {
                        "Final",
                        "Ambient occlusion",
                        "Specular occlusion",
                        "Shadow",
                        "Base color",
                        "Normal",
                        "Roughness",
                        "Metalness",
                        "World units",
                        "Barycentrics",
                        "Mesh index",
                        "Mip level (primary)",
                        "Mip level (specular)",
                    };

                    static const char* motionMode[] =
                    {
                        "Left / Right",
                        "Up / Down",
                        "Forward / Backward",
                    };

                    ImGui::Text("CAMERA (press RIGHT MOUSE BOTTON for free-fly mode)");
                    ImGui::Separator();
                    ImGui::SliderFloat("Field of view (deg)", &m_Settings.camFov, 10.0f, 150.0f);
                    ImGui::SliderFloat("Exposure", &m_Settings.exposure, 0.0001f, 1.0f, "%.7f", 5.0f);
                    ImGui::Combo("On screen", &m_Settings.onScreen, onScreenModes, helper::GetCountOf(onScreenModes));
                    if (m_Settings.reference)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                    ImGui::Checkbox("TAA", &m_Settings.temporal);
                    if (m_Settings.reference)
                        ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::Checkbox("3D motion vectors", &m_Settings.worldSpaceMotion);
                    ImGui::SameLine();
                    if (ImGui::Button("Emulate motion"))
                        m_Settings.motionStartTime = m_Settings.motionStartTime > 0.0 ? 0.0 : -1.0;
                    if (m_Settings.motionStartTime > 0.0)
                    {
                        ImGui::SliderFloat("Slower / Faster", &m_Settings.emulateMotionSpeed, -10.0f, 10.0f);
                        ImGui::SetNextItemWidth(160.0f);
                        ImGui::Combo("Mode", &m_Settings.motionMode, motionMode, helper::GetCountOf(motionMode));
                        ImGui::SameLine();
                        ImGui::Checkbox("Linear", &m_Settings.linear);
                    }
                }
                ImGui::PopID();
                ImGui::NewLine();
                ImGui::PushID("MATERIALS");
                {
                    static const char* forcedMaterial[] =
                    {
                        "None",
                        "Gypsum",
                        "Cobalt",
                    };

                    ImGui::Text("MATERIALS");
                    ImGui::Separator();
                    ImGui::SliderFloat2("Roughness / Metalness", &m_Settings.roughnessOverride, 0.0f, 1.0f, "%.3f", 2.0f);
                    ImGui::SetNextItemWidth(80.0f);
                    ImGui::Combo("Material", &m_Settings.forcedMaterial, forcedMaterial, helper::GetCountOf(forcedMaterial));
                    ImGui::SameLine();
                    ImGui::Checkbox("Full BRDF", &m_Settings.primaryFullBrdf);
                    ImGui::SameLine();
                    ImGui::Checkbox("Emission", &m_Settings.emission);
                    if (m_Settings.emission)
                        ImGui::SliderFloat("Emission intensity", &m_Settings.emissionIntensity, 1.0f, 100000.0f, "%.3f", 4.0f);
                }
                ImGui::PopID();

                if (m_Settings.onScreen == 8)
                    ImGui::SliderFloat("World units to meters", &m_Settings.unitsToMetersMultiplier, 0.0001f, 100.0f, "%.4f", 6.0f);
                else
                {
                    ImGui::NewLine();
                    ImGui::PushID("WORLD");
                    {
                        ImGui::Text("WORLD");
                        ImGui::Separator();
                        ImGui::SliderFloat2("Sun position (deg)", &m_Settings.sunAzimuth, -180.0f, 180.0f);
                        ImGui::SliderFloat("Sun angular size (deg)", &m_Settings.sunAngularDiameter, 0.0f, 3.0f);
                        ImGui::Checkbox("Animate sun", &m_Settings.animateSun);
                        ImGui::SameLine();
                        ImGui::Checkbox("Animate objects", &m_Settings.animatedObjects);
                        if (!m_Scene.animations.empty() && m_Scene.animations[m_Settings.activeAnimation].cameraNode.animationNodeID != -1)
                        {
                            ImGui::SameLine();
                            ImGui::Checkbox("Animate camera", &m_Settings.animateCamera);
                        }

                        if (m_Settings.animatedObjects)
                        {
                            ImGui::SliderFloat("Object scale", &m_Settings.animatedObjectScale, 0.1f, 2.0f);
                            if (!m_Settings.nineBrothers)
                                ImGui::SliderInt("Object number", &m_Settings.animatedObjectNum, 1, (int32_t)ANIMATED_INSTANCE_MAX_NUM);

                            ImGui::Checkbox("\"9 brothers\"", &m_Settings.nineBrothers);
                            ImGui::SameLine();
                            ImGui::Checkbox("Blink", &m_Settings.blink);
                            ImGui::SameLine();
                            ImGui::Checkbox("Emissive", &m_Settings.emissiveObjects);
                        }

                        if (m_Settings.animateSun || m_Settings.animatedObjects || !m_Scene.animations.empty())
                        {
                            if (m_Settings.animatedObjects)
                                ImGui::SameLine();
                            ImGui::Checkbox("Pause", &m_Settings.pauseAnimation);
                            ImGui::SliderFloat("Slower / Faster", &m_Settings.animationSpeed, -10.0f, 10.0f);
                        }

                        if (!m_Scene.animations.empty())
                        {
                            if (m_Scene.animations[m_Settings.activeAnimation].durationMs != 0.0f)
                            {
                                char animationLabel[128];
                                sprintf_s(animationLabel, "Animation %.1f sec (%%)", 0.001f * m_Scene.animations[m_Settings.activeAnimation].durationMs / (m_Settings.animationSpeed < 0.0f ? 1.0f / (1.0f + Abs(m_Settings.animationSpeed)) : (1.0f + m_Settings.animationSpeed)));
                                ImGui::SliderFloat(animationLabel, &m_Settings.animationProgress, 0.0f, 99.999f);

                                if (m_Scene.animations.size() > 1)
                                {
                                    char items[1024] = {'\0'};
                                    size_t offset = 0;
                                    char* iterator = items;
                                    for (auto animation : m_Scene.animations)
                                    {
                                        memcpy_s(iterator + offset, sizeof(items), animation.animationName.c_str(), animation.animationName.length() + 1);
                                        offset += animation.animationName.length() + 1;
                                    }
                                    ImGui::Combo("Animated scene", &m_Settings.activeAnimation, items, helper::GetCountOf(m_Scene.animations));
                                }
                            }
                        }

                        m_Settings.sunElevation = Clamp(m_Settings.sunElevation, -90.0f, 90.0f);
                    }
                    ImGui::PopID();
                    ImGui::NewLine();
                    ImGui::PushID("INDIRECT RAYS");
                    {
                        ImGui::Text("INDIRECT RAYS");
                        ImGui::Separator();
                        ImGui::SliderFloat("Sky ambient (%)", &m_Settings.skyAmbient, 0.0f, 20.0f, "%.3f", 2.0f);
                        ImGui::Checkbox("Full BRDF", &m_Settings.indirectFullBrdf);
                        ImGui::SameLine();
                        ImGui::Checkbox("0.5 rpp", &m_Settings.checkerboard);
                        ImGui::SameLine();
                        ImGui::Checkbox("Spec 2nd", &m_Settings.specSecondBounce);
                        ImGui::SameLine();
                        ImGui::Checkbox("Diff 2nd", &m_Settings.diffSecondBounce);
                    }
                    ImGui::PopID();
                    ImGui::NewLine();
                    ImGui::PushID("SWITCHES");
                    {
                        ImGui::Text("SWITCHES");
                        ImGui::Separator();
                        ImGui::Checkbox("Diffuse", &m_Settings.indirectDiffuse);
                        ImGui::SameLine();
                        ImGui::Checkbox("Reflections", &m_Settings.indirectSpecular);
                        ImGui::SameLine();
                        ImGui::Checkbox("Blue noise", &m_Settings.blueNoise);
                        ImGui::SameLine();
                        ImGui::Checkbox("Mip", &m_Settings.mip);
                        ImGui::Checkbox("Normal map", &m_Settings.normalMap);

                        if( m_Settings.metalnessOverride != 0.0f )
                        {
                            ImGui::SameLine();
                            ImGui::Checkbox("Metal ambient", &m_Settings.metalAmbient);
                        }
                    }
                    ImGui::PopID();
                    ImGui::NewLine();
                    ImGui::PushID("NRD");
                    {
                        const nrd::LibraryDesc& nrdLibraryDesc = nrd::GetLibraryDesc();

                        char nrd[64];
                        sprintf_s(nrd, "NRD v%u.%u.%u", nrdLibraryDesc.versionMajor, nrdLibraryDesc.versionMinor, nrdLibraryDesc.versionBuild);
                        ImGui::Text(nrd);
                        ImGui::Separator();
                        ImGui::SliderFloat("Disocclusion (%)", &m_Settings.disocclusionThreshold, 0.25f, 5.0f, "%.3f", 2.0f);
                        ImGui::SliderFloat("Antilag threshold", &m_Settings.antilagIntensityThreshold, 0.0f, 1.0f, "%.4f", 4.0f);
                        ImGui::Text("DIFFUSE:");
                        ImGui::PushID("DIFFUSE");
                        {
                            ImGui::SliderInt("History frames", &m_Settings.diffMaxHistoryFrameNum, 0, nrd::MAX_HISTORY_FRAME_NUM);
                            ImGui::SliderFloat("Hit distance range (m)", &m_Settings.diffHitDistScale, 0.0f, sceneRadius);
                            ImGui::SliderFloat("Blur radius (px)", &m_Settings.diffDenoisingRadius, 0.0f, 150.0f, "%.1f");
                            ImGui::SliderFloat("Adaptive radius scale", &m_Settings.diffAdaptiveRadiusScale, 0.0f, 10.0f, "%.3f");
                        }
                        ImGui::PopID();
                        ImGui::Text("SPECULAR:");
                        ImGui::PushID("SPECULAR");
                        {
                            ImGui::SliderInt("History frames", &m_Settings.specMaxHistoryFrameNum, 0, nrd::MAX_HISTORY_FRAME_NUM);
                            ImGui::SliderFloat("Hit distance range (m)", &m_Settings.specHitDistScale, 0.0f, sceneRadius);
                            ImGui::SliderFloat("Blur radius (px)", &m_Settings.specDenoisingRadius, 0.0f, 150.0f, "%.1f");
                            ImGui::SliderFloat("Adaptive radius scale", &m_Settings.specAdaptiveRadiusScale, 0.0f, 1.0f, "%.2f");
                            ImGui::Checkbox("Anisotropic", &m_Settings.specularAnisotropicFiltering);
                            ImGui::SameLine();
                            ImGui::Checkbox("Reference", &m_Settings.reference);
                            ImGui::SameLine();
                            ImGui::Checkbox("Antilag", &m_Settings.antilag);
                            ImGui::SameLine();
                            ImGui::Checkbox("Sync", &m_Settings.syncHistoryFrames);
                            if (m_Settings.syncHistoryFrames)
                                m_Settings.specMaxHistoryFrameNum = m_Settings.diffMaxHistoryFrameNum;
                        }
                        ImGui::PopID();
                    }
                    ImGui::PopID();
                    ImGui::NewLine();
                    ImGui::Separator();
                    ImGui::SliderFloat("Input / Denoised", &m_Settings.separator, 0.0f, 1.0f, "%.2f");
                    ImGui::SliderFloat("Debug", &m_Settings.debug, 0.0f, 1.0f);

                    isPressedReloadShaders = ImGui::Button("Reload shaders");

                    if (m_TestMode)
                    {
                        char s[512];
                        static uint32_t lastSelected = 0;

                        ImGui::NewLine();
                        ImGui::Text("TESTS");
                        ImGui::Separator();

                        char* p = s;
                        strcpy_s(s, m_SceneFile);
                        while (*p++)
                        {
                            if (*p == '/')
                            {
                                *p = '\0';
                                break;
                            }
                        }
                        strcat_s(s, "/tests.bin");

                        const std::string path = utils::GetFullPath(s, utils::DataFolder::SCENES);

                        static bool reloadNeeded = true;
                        static uint32_t itemNum = 0;
                        const uint32_t itemSize = sizeof(m_Settings) + m_Camera.GetDataSize();

                        if (reloadNeeded)
                        {
                            FILE* fp;
                            fopen_s(&fp, path.c_str(), "rb");

                            if (fp)
                            {
                                itemNum = _filelength( _fileno(fp) ) / itemSize;
                                fclose(fp);
                            }

                            reloadNeeded = false;
                        }

                        uint32_t i = 0;
                        for (; i < itemNum; i++)
                        {
                            char s[8];
                            sprintf_s(s, "%u", i + 1);
                            if( i % 14 != 0 )
                                ImGui::SameLine();
                            if (ImGui::Button(s, ImVec2(25.0f, 0.0f)))
                            {
                                FILE* fp;
                                fopen_s(&fp, path.c_str(), "rb");

                                if (fp)
                                {
                                    fseek(fp, i * itemSize, SEEK_SET);
                                    fread(&m_Settings, 1, sizeof(m_Settings), fp);
                                    fread(m_Camera.GetDataPtr(), 1, m_Camera.GetDataSize(), fp);
                                    fclose(fp);

                                    lastSelected = i + 1;
                                }
                            }
                        }

                        if (i % 14 != 0)
                            ImGui::SameLine();

                        if (ImGui::Button("Add"))
                        {
                            FILE* fp;
                            fopen_s(&fp, path.c_str(), "ab");

                            if (fp)
                            {
                                m_Settings.motionStartTime = m_Settings.motionStartTime > 0.0 ? -1.0 : 0.0;

                                fwrite(&m_Settings, 1, sizeof(m_Settings), fp);
                                fwrite(m_Camera.GetDataPtr(), 1, m_Camera.GetDataSize(), fp);
                                fclose(fp);

                                reloadNeeded = true;
                            }
                        }

                        if( (i + 1) % 14 != 0 )
                            ImGui::SameLine();

                        sprintf_s(s, "Del %u", lastSelected);
                        if (lastSelected != 0 && ImGui::Button(s))
                        {
                            std::vector<uint8_t> data;
                            utils::LoadFile(path, data);

                            FILE* fp;
                            fopen_s(&fp, path.c_str(), "wb");

                            if (fp)
                            {
                                for (i = 0; i < itemNum; i++)
                                {
                                    if (i != lastSelected - 1)
                                        fwrite(&data[i * itemSize], 1, itemSize, fp);
                                }

                                fclose(fp);

                                reloadNeeded = true;
                                itemNum--;
                                lastSelected = Min( lastSelected, itemNum );
                            }
                        }
                    }
                }
            }
        }
        ImGui::End();
    }

    // Update camera
    cBoxf cameraLimits = m_Scene.aabb;
    cameraLimits.Scale(2.0f);

    CameraDesc desc = {};
    desc.limits = cameraLimits;
    desc.aspectRatio = float( GetWindowWidth() ) / float( GetWindowHeight() );
    desc.horizontalFov = m_Settings.camFov;
    desc.nearZ = NEAR_Z / m_Settings.unitsToMetersMultiplier;
    desc.farZ = 1000.0f / m_Settings.unitsToMetersMultiplier;
    desc.isCustomMatrixSet = m_Settings.animateCamera;
    GetCameraDescFromInputDevices(desc);

    const float animationSpeed = m_Settings.pauseAnimation ? 0.0f : (m_Settings.animationSpeed < 0.0f ? 1.0f / (1.0f + Abs(m_Settings.animationSpeed)) : (1.0f + m_Settings.animationSpeed));
    const float scale = m_Settings.animatedObjectScale / (2.0f * m_Settings.unitsToMetersMultiplier);
    const float objectAnimationDelta = animationSpeed * m_Timer.GetElapsedTime() * 0.001f;

    if (m_Settings.motionStartTime > 0.0)
    {
        const float3 dirs[3] = { m_Camera.m_WorldToView.GetRow0().To3d(), m_Camera.m_WorldToView.GetRow1().To3d(), m_Camera.m_WorldToView.GetRow2().To3d() };
        float time = float(m_Timer.GetTimeStamp() - m_Settings.motionStartTime);
        float amplitude = 100.0f * m_Camera.m_MotionScale;
        float period = 0.0003f * time * (m_Settings.emulateMotionSpeed < 0.0f ? 1.0f / (1.0f + Abs(m_Settings.emulateMotionSpeed)) : (1.0f + m_Settings.emulateMotionSpeed));
        float3 strafeLocalPosition = dirs[m_Settings.motionMode] * amplitude * (m_Settings.linear ? WaveTriangle(period) - 0.5f : Sin( Pi(period) ) * 0.5f);

        desc.dUser = strafeLocalPosition - m_PrevLocalPos;
        m_PrevLocalPos = strafeLocalPosition;
    }
    else if (m_Settings.motionStartTime == -1.0)
    {
        m_Settings.motionStartTime = m_Timer.GetTimeStamp();
        m_PrevLocalPos = float3::Zero();
    }

    m_Scene.Animate(animationSpeed, m_Timer.GetElapsedTime(), m_Settings.animationProgress, m_Settings.activeAnimation, m_Settings.animateCamera ? &desc.customMatrix : nullptr);
    m_Camera.Update(desc, frameIndex);

    if (m_Settings.nineBrothers)
    {
        m_Settings.animatedObjectNum = 9;

        const float3& vRight = m_Camera.m_ViewToWorld.GetCol0().xmm;
        const float3& vTop = m_Camera.m_ViewToWorld.GetCol1().xmm;
        const float3& vForward = m_Camera.m_ViewToWorld.GetCol2().xmm;

        float3 basePos = ToFloat(m_Camera.m_GlobalPosition);

        for (int32_t i = -1; i <= 1; i++ )
        {
            for (int32_t j = -1; j <= 1; j++ )
            {
                const uint32_t index = (i + 1) * 3 + (j + 1);

                float x = float(i) * scale * 5.0f;
                float y = float(j) * scale * 5.0f;
                float z = 10.0f * scale * (CAMERA_LEFT_HANDED ? 1.0f : -1.0f);

                float3 pos = basePos + vRight * x + vTop * y + vForward * z;

                utils::Instance& instance = m_Scene.instances[ m_AnimatedInstances[index].instanceID ];
                instance.position = ToDouble( pos );
                instance.rotation = m_Camera.m_ViewToWorld;
                instance.rotation.SetTranslation( float3::Zero() );
                instance.rotation.AddScale(scale);
            }
        }
    }
    else if (m_Settings.animatedObjects)
    {
        for (int32_t i = 0; i < m_Settings.animatedObjectNum; i++)
        {
            float4x4 transform = m_AnimatedInstances[i].Animate(objectAnimationDelta, scale);

            utils::Instance& instance = m_Scene.instances[ m_AnimatedInstances[i].instanceID ];
            instance.rotation = transform;
            instance.position = m_AnimatedInstances[i].position;
        }
    }

    // Reload shaders
    if (isPressedReloadShaders)
        CreatePipelines();
}

void Sample::CreateSwapChain(nri::Format& swapChainFormat)
{
    nri::SwapChainDesc swapChainDesc = {};
    swapChainDesc.windowHandle = m_hWnd;
    swapChainDesc.commandQueue = m_CommandQueue;
    swapChainDesc.format = nri::SwapChainFormat::BT709_G22_8BIT;
    swapChainDesc.verticalSyncInterval = m_SwapInterval;
    swapChainDesc.width = (uint16_t)m_OutputResolution.x;
    swapChainDesc.height = (uint16_t)m_OutputResolution.y;
    swapChainDesc.textureNum = SWAP_CHAIN_TEXTURE_NUM;

    NRI_ABORT_ON_FAILURE(NRI.CreateSwapChain(*m_Device, swapChainDesc, m_SwapChain));

    uint32_t swapChainTextureNum = 0;
    nri::Texture* const* swapChainTextures = NRI.GetSwapChainTextures(*m_SwapChain, swapChainTextureNum, swapChainFormat);

    nri::ClearValueDesc clearColor = {};
    nri::FrameBufferDesc frameBufferDesc = {};
    frameBufferDesc.colorAttachmentNum = 1;
    frameBufferDesc.colorClearValues = &clearColor;

    for (uint32_t i = 0; i < swapChainTextureNum; i++)
    {
        m_SwapChainBuffers.emplace_back();
        BackBuffer& backBuffer = m_SwapChainBuffers.back();

        backBuffer = {};
        backBuffer.texture = swapChainTextures[i];

        nri::Texture2DViewDesc textureViewDesc = {backBuffer.texture, nri::Texture2DViewType::COLOR_ATTACHMENT, swapChainFormat};
        NRI_ABORT_ON_FAILURE(NRI.CreateTexture2DView(textureViewDesc, backBuffer.colorAttachment));

        frameBufferDesc.colorAttachments = &backBuffer.colorAttachment;
        NRI_ABORT_ON_FAILURE(NRI.CreateFrameBuffer(*m_Device, frameBufferDesc, backBuffer.frameBufferUI));
    }
}

void Sample::CreateCommandBuffers()
{
    for (Frame& frame : m_Frames)
    {
        NRI_ABORT_ON_FAILURE(NRI.CreateDeviceSemaphore(*m_Device, true, frame.deviceSemaphore));
        NRI_ABORT_ON_FAILURE(NRI.CreateCommandAllocator(*m_CommandQueue, nri::WHOLE_DEVICE_GROUP, frame.commandAllocator));
        for (nri::CommandBuffer*& commandBuffer : frame.commandBuffers)
            NRI_ABORT_ON_FAILURE(NRI.CreateCommandBuffer(*frame.commandAllocator, commandBuffer));
    }
}

void Sample::CreateTexture(std::vector<DescriptorDesc>& descriptorDescs, const char* debugName, nri::Format format, uint32_t width, uint32_t height, uint32_t mipNum, uint32_t arraySize, nri::TextureUsageBits usage, nri::AccessBits state)
{
    nri::Texture* texture = nullptr;
    const nri::CTextureDesc textureDesc = nri::CTextureDesc::Texture2D(format, width, height, mipNum, arraySize, usage);
    NRI_ABORT_ON_FAILURE(NRI.CreateTexture(*m_Device, textureDesc, texture));
    m_Textures.push_back(texture);

    if (state != nri::AccessBits::UNKNOWN)
    {
        nri::TextureTransitionBarrierDesc transition = nri::TextureTransition(texture, state, state == nri::AccessBits::SHADER_RESOURCE ? nri::TextureLayout::SHADER_RESOURCE : nri::TextureLayout::GENERAL);
        m_TextureStates.push_back(transition);
    }

    descriptorDescs.push_back( {debugName, texture, format, usage, nri::BufferUsageBits::NONE, arraySize > 1} );
}

void Sample::CreateBuffer(std::vector<DescriptorDesc>& descriptorDescs, const char* debugName, uint64_t elements, uint32_t stride, nri::BufferUsageBits usage, nri::Format format)
{
    nri::BufferDesc bufferDesc = {};
    bufferDesc.size = elements * stride;
    bufferDesc.structureStride = (format == nri::Format::UNKNOWN && stride != 1) ? stride : 0;
    bufferDesc.usageMask = usage;

    nri::Buffer* buffer = nullptr;
    NRI_ABORT_ON_FAILURE( NRI.CreateBuffer(*m_Device, bufferDesc, buffer) );
    m_Buffers.push_back(buffer);

    descriptorDescs.push_back( {debugName, buffer, format, nri::TextureUsageBits::NONE, usage} );
}

nri::Format GetCorrectFormatForDepth(const nri::Format& format)
{
    switch (format)
    {
    case nri::Format::D16_UNORM:                return nri::Format::R16_UNORM;
    case nri::Format::D24_UNORM_S8_UINT:        return nri::Format::R24_UNORM_X8;
    case nri::Format::D32_SFLOAT:               return nri::Format::R32_SFLOAT;
    case nri::Format::D32_SFLOAT_S8_UINT_X24:   return nri::Format::R32_SFLOAT_X8_X24;
    default: return format;
    }
}

void Sample::CreateDescriptors(const std::vector<DescriptorDesc>& descriptorDescs)
{
    nri::Descriptor* descriptor = nullptr;
    for (const DescriptorDesc& desc : descriptorDescs)
    {
        if (desc.textureUsage == nri::TextureUsageBits::NONE)
        {
            if (desc.bufferUsage == nri::BufferUsageBits::CONSTANT_BUFFER)
            {
                for (uint32_t i = 0; i < BUFFERED_FRAME_MAX_NUM; i++)
                {
                    nri::BufferViewDesc desc = {};
                    desc.buffer = Get(Buffer::GlobalConstants);
                    desc.viewType = nri::BufferViewType::CONSTANT;
                    desc.offset = i * m_ConstantBufferSize;
                    desc.size = m_ConstantBufferSize;

                    NRI_ABORT_ON_FAILURE( NRI.CreateBufferView(desc, m_Frames[i].globalConstantBufferDescriptor) );
                    m_Frames[i].globalConstantBufferOffset = desc.offset;
                }
            }
            else if (desc.bufferUsage & nri::BufferUsageBits::SHADER_RESOURCE)
            {
                const nri::BufferViewDesc viewDesc = {(nri::Buffer*)desc.resource, nri::BufferViewType::SHADER_RESOURCE, desc.format};
                NRI_ABORT_ON_FAILURE(NRI.CreateBufferView(viewDesc, descriptor));
                m_Descriptors.push_back(descriptor);
            }

            NRI.SetBufferDebugName(*(nri::Buffer*)desc.resource, desc.debugName);
        }
        else
        {
            nri::Format sanitizedFormat = GetCorrectFormatForDepth(desc.format);
            nri::Texture2DViewDesc viewDesc = {(nri::Texture*)desc.resource, desc.isArray ? nri::Texture2DViewType::SHADER_RESOURCE_2D_ARRAY : nri::Texture2DViewType::SHADER_RESOURCE_2D, sanitizedFormat};
            NRI_ABORT_ON_FAILURE(NRI.CreateTexture2DView(viewDesc, descriptor));
            m_Descriptors.push_back(descriptor);

            if (desc.textureUsage & nri::TextureUsageBits::SHADER_RESOURCE_STORAGE)
            {
                viewDesc.viewType = desc.isArray ? nri::Texture2DViewType::SHADER_RESOURCE_STORAGE_2D_ARRAY : nri::Texture2DViewType::SHADER_RESOURCE_STORAGE_2D;
                NRI_ABORT_ON_FAILURE(NRI.CreateTexture2DView(viewDesc, descriptor));
                m_Descriptors.push_back(descriptor);
            }

            NRI.SetTextureDebugName(*(nri::Texture*)desc.resource, desc.debugName);
        }
    }
}

void Sample::CreateResources(nri::Format swapChainFormat)
{
    std::vector<DescriptorDesc> descriptorDescs;

    const uint32_t w = m_RenderResolution.x;
    const uint32_t h = m_RenderResolution.y;
    const uint64_t instanceDataSize = (m_Scene.instances.size() + ANIMATED_INSTANCE_MAX_NUM) * sizeof(InstanceData);

    const uint64_t scratchBufferSize = NRI.GetAccelerationStructureBuildScratchBufferSize(*m_TLAS);

    // nri::MemoryLocation::HOST_UPLOAD
    CreateBuffer(descriptorDescs, "Buffer::GlobalConstants", m_ConstantBufferSize * BUFFERED_FRAME_MAX_NUM, 1, nri::BufferUsageBits::CONSTANT_BUFFER);
    CreateBuffer(descriptorDescs, "Buffer::TlasDataStaging", (m_Scene.instances.size() + ANIMATED_INSTANCE_MAX_NUM) * sizeof(nri::GeometryObjectInstance) * BUFFERED_FRAME_MAX_NUM, 1, nri::BufferUsageBits::NONE);
    CreateBuffer(descriptorDescs, "Buffer::InstanceDataStaging", instanceDataSize * BUFFERED_FRAME_MAX_NUM, 1, nri::BufferUsageBits::NONE);

    // nri::MemoryLocation::DEVICE
    CreateBuffer(descriptorDescs, "Buffer::ShaderTable", m_ShaderEntries.back(), 1, nri::BufferUsageBits::NONE);
    CreateBuffer(descriptorDescs, "Buffer::PrimitiveData", m_Scene.primitives.size(), sizeof(PrimitiveData), nri::BufferUsageBits::SHADER_RESOURCE, nri::Format::RGBA32_UINT);
    CreateBuffer(descriptorDescs, "Buffer::InstanceData", instanceDataSize / (4 * sizeof(float)), 4 * sizeof(float), nri::BufferUsageBits::SHADER_RESOURCE, nri::Format::RGBA32_SFLOAT);
    CreateBuffer(descriptorDescs, "Buffer::Scratch", scratchBufferSize, 1, nri::BufferUsageBits::RAY_TRACING_SCRATCH_BUFFER);

    CreateTexture(descriptorDescs, "Texture::IntegrateBRDF", nri::Format::RG16_SFLOAT, FG_TEX_SIZE, FG_TEX_SIZE, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE_STORAGE);
    CreateTexture(descriptorDescs, "Texture::ViewZ", nri::Format::R32_SFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::DirectLighting", nri::Format::R11_G11_B10_UFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::TransparentLighting", nri::Format::RGBA16_SFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::ObjectMotion", nri::Format::RGBA16_SFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::Normal_Roughness", nri::Format::RGBA8_UNORM, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::BaseColor_Metalness", nri::Format::RGBA8_UNORM, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::Shadow", nri::Format::R8_UNORM, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::DiffHit", nri::Format::RGBA16_SFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::SpecHit", nri::Format::RGBA16_SFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::Unfiltered_Shadow", nri::Format::RG16_SFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::Unfiltered_DiffA", nri::Format::RGBA16_SFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::Unfiltered_DiffB", nri::Format::RGBA16_SFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::Unfiltered_SpecHit", nri::Format::RGBA16_SFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::Composition", nri::Format::R10_G10_B10_A2_UNORM, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::CompositionHdr", nri::Format::RGBA16_SFLOAT, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE_STORAGE);
    CreateTexture(descriptorDescs, "Texture::TaaHistory", nri::Format::R10_G10_B10_A2_UNORM, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE);
    CreateTexture(descriptorDescs, "Texture::TaaHistoryPrev", nri::Format::R10_G10_B10_A2_UNORM, w, h, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::SHADER_RESOURCE_STORAGE);
    CreateTexture(descriptorDescs, "Texture::Final", swapChainFormat, m_OutputResolution.x, m_OutputResolution.y, 1, 1,
        nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, nri::AccessBits::COPY_SOURCE);

    // Material textures
    for (const utils::Texture* textureData : m_Scene.textures)
        CreateTexture(descriptorDescs, "", textureData->GetFormat(), textureData->GetWidth(), textureData->GetHeight(), textureData->GetMipNum(), textureData->GetArraySize(), nri::TextureUsageBits::SHADER_RESOURCE, nri::AccessBits::UNKNOWN);

    // Bind memory
    constexpr uint32_t offset = uint32_t(Buffer::UploadHeapBufferNum);
    NRI_ABORT_ON_FAILURE( helper::BindMemory(NRI, *m_Device, nri::MemoryLocation::HOST_UPLOAD, nullptr, 0, m_Buffers.data(), offset, m_Memories) );
    NRI_ABORT_ON_FAILURE( helper::BindMemory(NRI, *m_Device, nri::MemoryLocation::DEVICE, m_Textures.data(), helper::GetCountOf(m_Textures), m_Buffers.data() + offset, helper::GetCountOf(m_Buffers) - offset, m_Memories) );

    CreateDescriptors(descriptorDescs);
}

void Sample::CreatePipelines()
{
    if (!m_Pipelines.empty())
    {
        helper::WaitIdle(NRI, *m_Device, *m_CommandQueue);

        for (uint32_t i = 0; i < m_Pipelines.size(); i++)
            NRI.DestroyPipeline(*m_Pipelines[i]);
        m_Pipelines.clear();

        m_NRD.CreatePipelines();
    }

    utils::ShaderCodeStorage shaderCodeStorage;
    nri::PipelineLayout* pipelineLayout = nullptr;
    nri::Pipeline* pipeline = nullptr;

    nri::SamplerDesc samplerDescs[3] = {};
    {
        samplerDescs[0].addressModes = {nri::AddressMode::REPEAT, nri::AddressMode::REPEAT};
        samplerDescs[0].minification = nri::Filter::LINEAR;
        samplerDescs[0].magnification = nri::Filter::LINEAR;
        samplerDescs[0].mip = nri::Filter::LINEAR;
        samplerDescs[0].mipMax = 16.0f;

        samplerDescs[1].addressModes = {nri::AddressMode::REPEAT, nri::AddressMode::REPEAT};
        samplerDescs[1].minification = nri::Filter::NEAREST;
        samplerDescs[1].magnification = nri::Filter::NEAREST;
        samplerDescs[1].mip = nri::Filter::NEAREST;
        samplerDescs[1].mipMax = 16.0f;

        samplerDescs[2].addressModes = {nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE};
        samplerDescs[2].minification = nri::Filter::LINEAR;
        samplerDescs[2].magnification = nri::Filter::LINEAR;
    }

    const nri::DescriptorRangeDesc globalDescriptorRanges[] =
    {
        { 0, 1, nri::DescriptorType::CONSTANT_BUFFER, nri::ShaderStage::ALL },
    };

    const nri::StaticSamplerDesc staticSamplersDesc[] =
    {
        { samplerDescs[0], 1, nri::ShaderStage::ALL },
        { samplerDescs[1], 2, nri::ShaderStage::ALL },
        { samplerDescs[2], 3, nri::ShaderStage::ALL },
    };

    { // Pipeline::IntegrateBRDF
        const nri::DescriptorRangeDesc descriptorRanges[] =
        {
            { 0, 1, nri::DescriptorType::STORAGE_TEXTURE, nri::ShaderStage::ALL }
        };

        const nri::DescriptorSetDesc descriptorSetDesc[] =
        {
            { descriptorRanges, helper::GetCountOf(descriptorRanges) },
        };

        nri::PipelineLayoutDesc pipelineLayoutDesc = {};
        pipelineLayoutDesc.descriptorSetNum = helper::GetCountOf(descriptorSetDesc);
        pipelineLayoutDesc.descriptorSets = descriptorSetDesc;
        pipelineLayoutDesc.stageMask = nri::PipelineLayoutShaderStageBits::COMPUTE;

        NRI_ABORT_ON_FAILURE(NRI.CreatePipelineLayout(*m_Device, pipelineLayoutDesc, pipelineLayout));
        m_PipelineLayouts.push_back(pipelineLayout);

        nri::ComputePipelineDesc pipelineDesc = {};
        pipelineDesc.pipelineLayout = pipelineLayout;
        pipelineDesc.computeShader = utils::LoadShader(m_DeviceDesc->graphicsAPI, "09_IntegrateBRDF.cs", shaderCodeStorage);

        NRI_ABORT_ON_FAILURE(NRI.CreateComputePipeline(*m_Device, pipelineDesc, pipeline));
        m_Pipelines.push_back(pipeline);
    }

    { // Pipeline::Raytracing
        const nri::DescriptorRangeDesc descriptorRanges1[] =
        {
            { 0, 5, nri::DescriptorType::TEXTURE, nri::ShaderStage::RAYGEN },
            { 5, 10, nri::DescriptorType::STORAGE_TEXTURE, nri::ShaderStage::RAYGEN },
        };

        const uint32_t textureNum = helper::GetCountOf(m_Scene.materials) * TEXTURES_PER_MATERIAL;
        nri::DescriptorRangeDesc descriptorRanges2[] =
        {
            { 0, 1, nri::DescriptorType::ACCELERATION_STRUCTURE, nri::ShaderStage::RAYGEN },
            { 1, 2, nri::DescriptorType::BUFFER, nri::ShaderStage::ALL },
            { 3, textureNum, nri::DescriptorType::TEXTURE, nri::ShaderStage::ALL, helper::VARIABLE_DESCRIPTOR_NUM },
        };

        const nri::DescriptorSetDesc descriptorSetDesc[] =
        {
            { globalDescriptorRanges, helper::GetCountOf(globalDescriptorRanges), staticSamplersDesc, helper::GetCountOf(staticSamplersDesc) },
            { descriptorRanges1, helper::GetCountOf(descriptorRanges1) },
            { descriptorRanges2, helper::GetCountOf(descriptorRanges2) }
        };

        nri::PipelineLayoutDesc pipelineLayoutDesc = {};
        pipelineLayoutDesc.descriptorSets = descriptorSetDesc;
        pipelineLayoutDesc.descriptorSetNum = helper::GetCountOf(descriptorSetDesc);
        pipelineLayoutDesc.stageMask = nri::PipelineLayoutShaderStageBits::ALL_RAY_TRACING;

        NRI_ABORT_ON_FAILURE(NRI.CreatePipelineLayout(*m_Device, pipelineLayoutDesc, pipelineLayout));
        m_PipelineLayouts.push_back(pipelineLayout);

        const nri::ShaderDesc shaderDescs[] =
        {
            utils::LoadShader(m_DeviceDesc->graphicsAPI, "09_Raytracing00.rgen", shaderCodeStorage, "Raytracing00_rgen"),
            utils::LoadShader(m_DeviceDesc->graphicsAPI, "09_Raytracing01.rgen", shaderCodeStorage, "Raytracing01_rgen"),
            utils::LoadShader(m_DeviceDesc->graphicsAPI, "09_Raytracing10.rgen", shaderCodeStorage, "Raytracing10_rgen"),
            utils::LoadShader(m_DeviceDesc->graphicsAPI, "09_Raytracing11.rgen", shaderCodeStorage, "Raytracing11_rgen"),
            utils::LoadShader(m_DeviceDesc->graphicsAPI, "09_Main.rmiss", shaderCodeStorage, "Main_rmiss"),
            utils::LoadShader(m_DeviceDesc->graphicsAPI, "09_Main.rchit", shaderCodeStorage, "Main_rchit"),
            utils::LoadShader(m_DeviceDesc->graphicsAPI, "09_Main.rahit", shaderCodeStorage, "Main_rahit"),
        };

        nri::ShaderLibrary shaderLibrary = {};
        shaderLibrary.shaderDescs = shaderDescs;
        shaderLibrary.shaderNum = helper::GetCountOf(shaderDescs);

        const nri::ShaderGroupDesc shaderGroupDescs[] =
        {
            { 1 },          // Raytracing00_rgen - checkerboard = 0, 2nd bounce specular = 0
            { 2 },          // Raytracing01_rgen - checkerboard = 0, 2nd bounce specular = 1
            { 3 },          // Raytracing10_rgen - checkerboard = 1, 2nd bounce specular = 0
            { 4 },          // Raytracing11_rgen - checkerboard = 1, 2nd bounce specular = 1
            { 5 },          // Main_rmiss
            { 6, 7 },       // Main_rhit
        };

        nri::RayTracingPipelineDesc pipelineDesc = {};
        pipelineDesc.recursionDepthMax = 1;
        pipelineDesc.payloadAttributeSizeMax = 4 * sizeof(uint32_t);
        pipelineDesc.intersectionAttributeSizeMax = 2 * sizeof(float);
        pipelineDesc.pipelineLayout = pipelineLayout;
        pipelineDesc.shaderGroupDescs = shaderGroupDescs;
        pipelineDesc.shaderGroupDescNum = helper::GetCountOf(shaderGroupDescs);
        pipelineDesc.shaderLibrary = &shaderLibrary;

        NRI_ABORT_ON_FAILURE(NRI.CreateRayTracingPipeline(*m_Device, pipelineDesc, pipeline));
        m_Pipelines.push_back(pipeline);
    }

    { // Pipeline::Composition
        const nri::DescriptorRangeDesc descriptorRanges[] =
        {
            { 0, 12, nri::DescriptorType::TEXTURE, nri::ShaderStage::ALL },
            { 12, 2, nri::DescriptorType::STORAGE_TEXTURE, nri::ShaderStage::ALL }
        };

        const nri::DescriptorSetDesc descriptorSetDesc[] =
        {
            { globalDescriptorRanges, helper::GetCountOf(globalDescriptorRanges), staticSamplersDesc, helper::GetCountOf(staticSamplersDesc) },
            { descriptorRanges, helper::GetCountOf(descriptorRanges) },
        };

        nri::PipelineLayoutDesc pipelineLayoutDesc = {};
        pipelineLayoutDesc.descriptorSets = descriptorSetDesc;
        pipelineLayoutDesc.descriptorSetNum = helper::GetCountOf(descriptorSetDesc);
        pipelineLayoutDesc.stageMask = nri::PipelineLayoutShaderStageBits::COMPUTE;

        NRI_ABORT_ON_FAILURE(NRI.CreatePipelineLayout(*m_Device, pipelineLayoutDesc, pipelineLayout));
        m_PipelineLayouts.push_back(pipelineLayout);

        nri::ComputePipelineDesc pipelineDesc = {};
        pipelineDesc.pipelineLayout = pipelineLayout;
        pipelineDesc.computeShader = utils::LoadShader(m_DeviceDesc->graphicsAPI, "09_Composition.cs", shaderCodeStorage);

        NRI_ABORT_ON_FAILURE(NRI.CreateComputePipeline(*m_Device, pipelineDesc, pipeline));
        m_Pipelines.push_back(pipeline);
    }

    { // Pipeline::Temporal
        const nri::DescriptorRangeDesc descriptorRanges[] =
        {
            { 0, 4, nri::DescriptorType::TEXTURE, nri::ShaderStage::ALL },
            { 4, 2, nri::DescriptorType::STORAGE_TEXTURE, nri::ShaderStage::ALL }
        };

        const nri::DescriptorSetDesc descriptorSetDesc[] =
        {
            { globalDescriptorRanges, helper::GetCountOf(globalDescriptorRanges), staticSamplersDesc, helper::GetCountOf(staticSamplersDesc) },
            { descriptorRanges, helper::GetCountOf(descriptorRanges) },
        };

        nri::PipelineLayoutDesc pipelineLayoutDesc = {};
        pipelineLayoutDesc.descriptorSets = descriptorSetDesc;
        pipelineLayoutDesc.descriptorSetNum = helper::GetCountOf(descriptorSetDesc);
        pipelineLayoutDesc.stageMask = nri::PipelineLayoutShaderStageBits::COMPUTE;

        NRI_ABORT_ON_FAILURE(NRI.CreatePipelineLayout(*m_Device, pipelineLayoutDesc, pipelineLayout));
        m_PipelineLayouts.push_back(pipelineLayout);

        nri::ComputePipelineDesc pipelineDesc = {};
        pipelineDesc.pipelineLayout = pipelineLayout;
        pipelineDesc.computeShader = utils::LoadShader(m_DeviceDesc->graphicsAPI, "09_Temporal.cs", shaderCodeStorage);

        NRI_ABORT_ON_FAILURE(NRI.CreateComputePipeline(*m_Device, pipelineDesc, pipeline));
        m_Pipelines.push_back(pipeline);
    }

    // Raygen shaders
    uint64_t shaderGroupOffset = 0;
    m_ShaderEntries.push_back(shaderGroupOffset); shaderGroupOffset += m_DeviceDesc->rayTracingShaderGroupIdentifierSize; //ShaderGroup::Raytracing00_rgen

    shaderGroupOffset = helper::GetAlignedSize(shaderGroupOffset, m_DeviceDesc->rayTracingShaderTableAligment);
    m_ShaderEntries.push_back(shaderGroupOffset); shaderGroupOffset += m_DeviceDesc->rayTracingShaderGroupIdentifierSize; //ShaderGroup::Raytracing01_rgen

    shaderGroupOffset = helper::GetAlignedSize(shaderGroupOffset, m_DeviceDesc->rayTracingShaderTableAligment);
    m_ShaderEntries.push_back(shaderGroupOffset); shaderGroupOffset += m_DeviceDesc->rayTracingShaderGroupIdentifierSize; //ShaderGroup::Raytracing10_rgen

    shaderGroupOffset = helper::GetAlignedSize(shaderGroupOffset, m_DeviceDesc->rayTracingShaderTableAligment);
    m_ShaderEntries.push_back(shaderGroupOffset); shaderGroupOffset += m_DeviceDesc->rayTracingShaderGroupIdentifierSize; //ShaderGroup::Raytracing11_rgen

    // Miss shaders
    shaderGroupOffset = helper::GetAlignedSize(shaderGroupOffset, m_DeviceDesc->rayTracingShaderTableAligment);
    m_ShaderEntries.push_back(shaderGroupOffset); shaderGroupOffset += m_DeviceDesc->rayTracingShaderGroupIdentifierSize; //ShaderGroup::Main_rmiss

    // Hit shader groups
    shaderGroupOffset = helper::GetAlignedSize(shaderGroupOffset, m_DeviceDesc->rayTracingShaderTableAligment);
    m_ShaderEntries.push_back(shaderGroupOffset); shaderGroupOffset += m_DeviceDesc->rayTracingShaderGroupIdentifierSize; //ShaderGroup::Main_rhit

    // Total size
    m_ShaderEntries.push_back(shaderGroupOffset);
}

void Sample::CreateDescriptorSets()
{
    nri::DescriptorSet* descriptorSet = nullptr;

    nri::DescriptorPoolDesc descriptorPoolDesc = {};
    descriptorPoolDesc.descriptorSetMaxNum = 128;
    descriptorPoolDesc.staticSamplerMaxNum = 3 * BUFFERED_FRAME_MAX_NUM;
    descriptorPoolDesc.storageTextureMaxNum = 128;
    descriptorPoolDesc.textureMaxNum = 128 + uint32_t(m_Scene.materials.size()) * TEXTURES_PER_MATERIAL;
    descriptorPoolDesc.accelerationStructureMaxNum = 1 * BUFFERED_FRAME_MAX_NUM;
    descriptorPoolDesc.bufferMaxNum = 16;
    descriptorPoolDesc.constantBufferMaxNum = 1 * BUFFERED_FRAME_MAX_NUM;
    NRI_ABORT_ON_FAILURE(NRI.CreateDescriptorPool(*m_Device, descriptorPoolDesc, m_DescriptorPool));

    // Constant buffer
    for (Frame& frame : m_Frames)
    {
        NRI_ABORT_ON_FAILURE(NRI.AllocateDescriptorSets(*m_DescriptorPool, *GetPipelineLayout(Pipeline::Raytracing), 0, &frame.globalConstantBufferDescriptorSet, 1, nri::WHOLE_DEVICE_GROUP, 0));

        const nri::DescriptorRangeUpdateDesc descriptorRangeUpdateDesc[] =
        {
            { &frame.globalConstantBufferDescriptor, 1 },
        };

        NRI.UpdateDescriptorRanges(*frame.globalConstantBufferDescriptorSet, nri::WHOLE_DEVICE_GROUP, 0, helper::GetCountOf(descriptorRangeUpdateDesc), descriptorRangeUpdateDesc);
    }

    { // DescriptorSet::IntegrateBRDF0
        NRI_ABORT_ON_FAILURE(NRI.AllocateDescriptorSets(*m_DescriptorPool, *GetPipelineLayout(Pipeline::IntegrateBRDF), 0, &descriptorSet, 1, nri::WHOLE_DEVICE_GROUP, 0));
        m_DescriptorSets.push_back(descriptorSet);

        const nri::Descriptor* storageTextures[] =
        {
            Get(Descriptor::IntegrateBRDF_StorageTexture),
        };

        const nri::DescriptorRangeUpdateDesc descriptorRangeUpdateDesc[] =
        {
            { storageTextures, helper::GetCountOf(storageTextures) },
        };

        NRI.UpdateDescriptorRanges(*descriptorSet, nri::WHOLE_DEVICE_GROUP, 0, helper::GetCountOf(descriptorRangeUpdateDesc), descriptorRangeUpdateDesc);
    }

    { // DescriptorSet::Raytracing2
        std::vector<nri::Descriptor*> textures(m_Scene.materials.size() * TEXTURES_PER_MATERIAL);
        for (size_t i = 0; i < m_Scene.materials.size(); i++)
        {
            const uint32_t index = uint32_t(i) * TEXTURES_PER_MATERIAL;
            const utils::Material& material = m_Scene.materials[i];

            textures[index] = Get( Descriptor((uint32_t)Descriptor::MaterialTextures + material.diffuseMapIndex) );
            textures[index + 1] = Get( Descriptor((uint32_t)Descriptor::MaterialTextures + material.specularMapIndex) );
            textures[index + 2] = Get( Descriptor((uint32_t)Descriptor::MaterialTextures + material.normalMapIndex) );
            textures[index + 3] = Get( Descriptor((uint32_t)Descriptor::MaterialTextures + material.emissiveMapIndex) );
        }

        NRI_ABORT_ON_FAILURE(NRI.AllocateDescriptorSets(*m_DescriptorPool, *GetPipelineLayout(Pipeline::Raytracing), 2, &descriptorSet, 1, nri::WHOLE_DEVICE_GROUP, helper::GetCountOf(textures)));
        m_DescriptorSets.push_back(descriptorSet);

        const nri::Descriptor* buffers[] =
        {
            Get(Descriptor::PrimitiveData_Buffer),
            Get(Descriptor::InstanceData_Buffer)
        };

        const nri::DescriptorRangeUpdateDesc descriptorRangeUpdateDesc[] =
        {
            { &Get(Descriptor::Opaque_AccelerationStructure), 1 },
            { buffers, helper::GetCountOf(buffers) },
            { textures.data(), helper::GetCountOf(textures) }
        };
        NRI.UpdateDescriptorRanges(*descriptorSet, nri::WHOLE_DEVICE_GROUP, 0, helper::GetCountOf(descriptorRangeUpdateDesc), descriptorRangeUpdateDesc);
    }

    { // DescriptorSet::Raytracing1
        NRI_ABORT_ON_FAILURE(NRI.AllocateDescriptorSets(*m_DescriptorPool, *GetPipelineLayout(Pipeline::Raytracing), 1, &descriptorSet, 1, nri::WHOLE_DEVICE_GROUP, 0));
        m_DescriptorSets.push_back(descriptorSet);

        const nri::Descriptor* textures[] =
        {
            Get( Descriptor((uint32_t)Descriptor::MaterialTextures + utils::StaticTexture::ScramblingRanking1spp) ),
            Get( Descriptor((uint32_t)Descriptor::MaterialTextures + utils::StaticTexture::ScramblingRanking32spp) ),
            Get( Descriptor((uint32_t)Descriptor::MaterialTextures + utils::StaticTexture::SobolSequence) ),
            Get(Descriptor::IntegrateBRDF_Texture),
            Get(Descriptor::CompositionHdr_Texture),
        };

        const nri::Descriptor* storageTextures[] =
        {
            Get(Descriptor::DirectLighting_StorageTexture),
            Get(Descriptor::TransparentLighting_StorageTexture),
            Get(Descriptor::ObjectMotion_StorageTexture),
            Get(Descriptor::ViewZ_StorageTexture),
            Get(Descriptor::Normal_Roughness_StorageTexture),
            Get(Descriptor::BaseColor_Metalness_StorageTexture),
            Get(Descriptor::Unfiltered_Shadow_StorageTexture),
            Get(Descriptor::Unfiltered_DiffA_StorageTexture),
            Get(Descriptor::Unfiltered_DiffB_StorageTexture),
            Get(Descriptor::Unfiltered_SpecHit_StorageTexture),
        };

        const nri::DescriptorRangeUpdateDesc descriptorRangeUpdateDesc[] =
        {
            { textures, helper::GetCountOf(textures) },
            { storageTextures, helper::GetCountOf(storageTextures) },
        };

        NRI.UpdateDescriptorRanges(*descriptorSet, nri::WHOLE_DEVICE_GROUP, 0, helper::GetCountOf(descriptorRangeUpdateDesc), descriptorRangeUpdateDesc);
    }

    { // DescriptorSet::Composition1
        NRI_ABORT_ON_FAILURE(NRI.AllocateDescriptorSets(*m_DescriptorPool, *GetPipelineLayout(Pipeline::Composition), 1, &descriptorSet, 1, nri::WHOLE_DEVICE_GROUP, 0));
        m_DescriptorSets.push_back(descriptorSet);

        const nri::Descriptor* textures[] =
        {
            Get(Descriptor::DirectLighting_Texture),
            Get(Descriptor::TransparentLighting_Texture),
            Get(Descriptor::Normal_Roughness_Texture),
            Get(Descriptor::BaseColor_Metalness_Texture),
            Get(Descriptor::Shadow_Texture),
            Get(Descriptor::DiffHit_Texture),
            Get(Descriptor::SpecHit_Texture),
            Get(Descriptor::Unfiltered_Shadow_Texture),
            Get(Descriptor::Unfiltered_DiffA_Texture),
            Get(Descriptor::Unfiltered_DiffB_Texture),
            Get(Descriptor::Unfiltered_SpecHit_Texture),
            Get(Descriptor::IntegrateBRDF_Texture),
        };

        const nri::Descriptor* storageTextures[] =
        {
            Get(Descriptor::Composition_StorageTexture),
            Get(Descriptor::CompositionHdr_StorageTexture),
        };

        const nri::DescriptorRangeUpdateDesc descriptorRangeUpdateDesc[] =
        {
            { textures, helper::GetCountOf(textures) },
            { storageTextures, helper::GetCountOf(storageTextures) },
        };

        NRI.UpdateDescriptorRanges(*descriptorSet, nri::WHOLE_DEVICE_GROUP, 0, helper::GetCountOf(descriptorRangeUpdateDesc), descriptorRangeUpdateDesc);
    }

    { // DescriptorSet::Temporal1a
        NRI_ABORT_ON_FAILURE(NRI.AllocateDescriptorSets(*m_DescriptorPool, *GetPipelineLayout(Pipeline::Temporal), 1, &descriptorSet, 1, nri::WHOLE_DEVICE_GROUP, 0));
        m_DescriptorSets.push_back(descriptorSet);

        const nri::Descriptor* textures[] =
        {
            Get(Descriptor::ViewZ_Texture),
            Get(Descriptor::ObjectMotion_Texture),
            Get(Descriptor::Composition_Texture),
            Get(Descriptor::TaaHistoryPrev_Texture),
        };

        const nri::Descriptor* storageTextures[] =
        {
            Get(Descriptor::TaaHistory_StorageTexture),
            Get(Descriptor::Final_StorageTexture),
        };

        const nri::DescriptorRangeUpdateDesc descriptorRangeUpdateDesc[] =
        {
            { textures, helper::GetCountOf(textures) },
            { storageTextures, helper::GetCountOf(storageTextures) },
        };

        NRI.UpdateDescriptorRanges(*descriptorSet, nri::WHOLE_DEVICE_GROUP, 0, helper::GetCountOf(descriptorRangeUpdateDesc), descriptorRangeUpdateDesc);
    }

    { // DescriptorSet::Temporal1b
        NRI_ABORT_ON_FAILURE(NRI.AllocateDescriptorSets(*m_DescriptorPool, *GetPipelineLayout(Pipeline::Temporal), 1, &descriptorSet, 1, nri::WHOLE_DEVICE_GROUP, 0));
        m_DescriptorSets.push_back(descriptorSet);

        const nri::Descriptor* textures[] =
        {
            Get(Descriptor::ViewZ_Texture),
            Get(Descriptor::ObjectMotion_Texture),
            Get(Descriptor::Composition_Texture),
            Get(Descriptor::TaaHistory_Texture),
        };

        const nri::Descriptor* storageTextures[] =
        {
            Get(Descriptor::TaaHistoryPrev_StorageTexture),
            Get(Descriptor::Final_StorageTexture),
        };

        const nri::DescriptorRangeUpdateDesc descriptorRangeUpdateDesc[] =
        {
            { textures, helper::GetCountOf(textures) },
            { storageTextures, helper::GetCountOf(storageTextures) },
        };

        NRI.UpdateDescriptorRanges(*descriptorSet, nri::WHOLE_DEVICE_GROUP, 0, helper::GetCountOf(descriptorRangeUpdateDesc), descriptorRangeUpdateDesc);
    }
}

void Sample::UploadStaticData()
{
    // PrimitiveData
    const size_t triangleNum = m_Scene.primitives.size();
    std::vector<PrimitiveData> primitiveData(triangleNum);
    uint32_t n = 0;
    for (const utils::Mesh& mesh : m_Scene.meshes)
    {
        uint32_t triangleNum = mesh.indexNum / 3;
        for (uint32_t j = 0; j < triangleNum; j++)
        {
            uint32_t primitiveIndex = mesh.indexOffset / 3 + j;
            const utils::Primitive& primitive = m_Scene.primitives[primitiveIndex];

            const utils::Vertex& v0 = m_Scene.vertices[ mesh.vertexOffset + m_Scene.indices[primitiveIndex * 3] ];
            const utils::Vertex& v1 = m_Scene.vertices[ mesh.vertexOffset + m_Scene.indices[primitiveIndex * 3 + 1] ];
            const utils::Vertex& v2 = m_Scene.vertices[ mesh.vertexOffset + m_Scene.indices[primitiveIndex * 3 + 2] ];

            float4 n0 = Packed::uint_to_uf4<10, 10, 10, 2>(v0.normal);
            float4 n1 = Packed::uint_to_uf4<10, 10, 10, 2>(v1.normal);
            float4 n2 = Packed::uint_to_uf4<10, 10, 10, 2>(v2.normal);
            float4 t0 = Packed::uint_to_uf4<10, 10, 10, 2>(v0.tangent);
            float4 t1 = Packed::uint_to_uf4<10, 10, 10, 2>(v1.tangent);
            float4 t2 = Packed::uint_to_uf4<10, 10, 10, 2>(v2.tangent);
            float4 nf = Packed::uint_to_uf4<10, 10, 10, 2>(primitive.normal);

            float3 n0v = Normalize(float3(n0.xmm) * 2.0f - 1.0f);
            float3 n1v = Normalize(float3(n1.xmm) * 2.0f - 1.0f);
            float3 n2v = Normalize(float3(n2.xmm) * 2.0f - 1.0f);
            float3 t0v = Normalize(float3(t0.xmm) * 2.0f - 1.0f);
            float3 t1v = Normalize(float3(t1.xmm) * 2.0f - 1.0f);
            float3 t2v = Normalize(float3(t2.xmm) * 2.0f - 1.0f);
            float3 nfv = Normalize(float3(nf.xmm) * 2.0f - 1.0f);

            PrimitiveData& data = primitiveData[n++];
            data.uv0 = v0.uv;
            data.uv1 = v1.uv;
            data.uv2 = v2.uv;
            data.fnX_fnY = Packed::sf2_to_h2(nfv.x, nfv.y);

            data.fnZ_worldToUvUnits = Packed::sf2_to_h2(nfv.z, primitive.worldToUvUnits);
            data.n0X_n0Y = Packed::sf2_to_h2(n0v.x, n0v.y);
            data.n0Z_n1X = Packed::sf2_to_h2(n0v.z, n1v.x);
            data.n1Y_n1Z = Packed::sf2_to_h2(n1v.y, n1v.z);

            data.n2X_n2Y = Packed::sf2_to_h2(n2v.x, n2v.y);
            data.n2Z_t0X = Packed::sf2_to_h2(n2v.z, t0v.x);
            data.t0Y_t0Z = Packed::sf2_to_h2(t0v.y, t0v.z);
            data.t1X_t1Y = Packed::sf2_to_h2(t1v.x, t1v.y);

            data.t1Z_t2X = Packed::sf2_to_h2(t1v.z, t2v.x);
            data.t2Y_t2Z = Packed::sf2_to_h2(t2v.y, t2v.z);
            data.b0S_b1S = Packed::sf2_to_h2(t0.w, t1.w);
            data.b2S = Packed::sf2_to_h2(t2.w, 0.0f);
        }
    }

    // MaterialTextures
    uint32_t subresourceNum = 0;
    for (const utils::Texture* texture : m_Scene.textures)
        subresourceNum += texture->GetArraySize() * texture->GetMipNum();

    std::vector<helper::TextureDataDesc> textureData( m_Scene.textures.size() );
    std::vector<helper::TextureSubresource> subresources( subresourceNum );
    uint32_t subresourceOffset = 0;

    helper::TextureDataDesc* textureDataDesc = textureData.data();
    for (const utils::Texture* texture : m_Scene.textures)
    {
        for (uint32_t layer = 0; layer < texture->GetArraySize(); layer++)
            for (uint32_t mip = 0; mip < texture->GetMipNum(); mip++)
                texture->GetSubresource(subresources[subresourceOffset + layer * texture->GetMipNum() + mip], mip, layer);

        textureDataDesc->subresources = &subresources[subresourceOffset];
        textureDataDesc->mipNum = texture->GetMipNum();
        textureDataDesc->arraySize = texture->GetArraySize();
        textureDataDesc->texture = Get( (Texture)((uint32_t)Texture::MaterialTextures + textureDataDesc - textureData.data()) );
        textureDataDesc->nextLayout = nri::TextureLayout::SHADER_RESOURCE;
        textureDataDesc->nextAccess = nri::AccessBits::SHADER_RESOURCE;
        textureDataDesc++;

        subresourceOffset += texture->GetArraySize() * texture->GetMipNum();
    }

    for (const nri::TextureTransitionBarrierDesc& state : m_TextureStates)
    {
        helper::TextureDataDesc desc = {};
        desc.nextAccess = state.nextAccess;
        desc.nextLayout = state.nextLayout;
        desc.texture = (nri::Texture*)state.texture;

        textureData.push_back(desc);
    }

    // Buffer data
    helper::BufferDataDesc dataDescArray[] =
    {
        { primitiveData.data(), helper::GetByteSizeOf(primitiveData), Get(Buffer::PrimitiveData), 0, nri::AccessBits::SHADER_RESOURCE },
    };

    NRI_ABORT_ON_FAILURE(helper::UploadData(NRI, *m_Device, textureData.data(), helper::GetCountOf(textureData), dataDescArray, helper::GetCountOf(dataDescArray)));
}

void Sample::CreateBottomLevelAccelerationStructures()
{
    for (const utils::Mesh& mesh : m_Scene.meshes)
    {
        const uint64_t vertexDataSize = mesh.vertexNum * sizeof(utils::Vertex);
        const uint64_t indexDataSize = mesh.indexNum * sizeof(utils::Index);

        nri::Buffer* tempBuffer = nullptr;
        nri::Memory* tempMemory = nullptr;
        CreateUploadBuffer(vertexDataSize + indexDataSize, tempBuffer, tempMemory);

        uint8_t* data = (uint8_t*)NRI.MapBuffer(*tempBuffer, 0, nri::WHOLE_SIZE);
        memcpy(data, &m_Scene.vertices[mesh.vertexOffset], vertexDataSize);
        memcpy(data + vertexDataSize, &m_Scene.indices[mesh.indexOffset], indexDataSize);
        NRI.UnmapBuffer(*tempBuffer);

        nri::GeometryObject geometryObject = {};
        geometryObject.type = nri::GeometryType::TRIANGLES;
        geometryObject.flags = nri::BottomLevelGeometryBits::NONE;
        geometryObject.triangles.vertexBuffer = tempBuffer;
        geometryObject.triangles.vertexOffset = 0;
        geometryObject.triangles.vertexNum = mesh.vertexNum;
        geometryObject.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
        geometryObject.triangles.vertexStride = sizeof(utils::Vertex);
        geometryObject.triangles.indexBuffer = tempBuffer;
        geometryObject.triangles.indexOffset = vertexDataSize;
        geometryObject.triangles.indexNum = mesh.indexNum;
        geometryObject.triangles.indexType = sizeof(utils::Index) == 2 ? nri::IndexType::UINT16 : nri::IndexType::UINT32;

        nri::AccelerationStructureDesc blasDesc = {};
        blasDesc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
        blasDesc.flags = BUILD_FLAGS;
        blasDesc.instanceOrGeometryObjectNum = 1;
        blasDesc.geometryObjects = &geometryObject;

        nri::AccelerationStructure* blas = nullptr;
        NRI_ABORT_ON_FAILURE(NRI.CreateAccelerationStructure(*m_Device, blasDesc, blas));
        m_BLASs.push_back(blas);

        nri::MemoryDesc memoryDesc = {};
        NRI.GetAccelerationStructureMemoryInfo(*blas, memoryDesc);

        nri::Memory* memory = nullptr;
        NRI_ABORT_ON_FAILURE(NRI.AllocateMemory(*m_Device, nri::WHOLE_DEVICE_GROUP, memoryDesc.type, memoryDesc.size, memory));
        m_Memories.push_back(memory);

        const nri::AccelerationStructureMemoryBindingDesc memoryBindingDesc = { memory, blas };
        NRI_ABORT_ON_FAILURE(NRI.BindAccelerationStructureMemory(*m_Device, &memoryBindingDesc, 1));

        BuildBottomLevelAccelerationStructure(*blas, &geometryObject, 1);

        NRI.DestroyBuffer(*tempBuffer);
        NRI.FreeMemory(*tempMemory);
    }
}

void Sample::CreateTopLevelAccelerationStructure()
{
    nri::AccelerationStructureDesc tlasDesc = {};
    tlasDesc.type = nri::AccelerationStructureType::TOP_LEVEL;
    tlasDesc.flags = BUILD_FLAGS;
    tlasDesc.instanceOrGeometryObjectNum = helper::GetCountOf(m_Scene.instances) + ANIMATED_INSTANCE_MAX_NUM;

    NRI_ABORT_ON_FAILURE(NRI.CreateAccelerationStructure(*m_Device, tlasDesc, m_TLAS));

    nri::MemoryDesc memoryDesc = {};
    NRI.GetAccelerationStructureMemoryInfo(*m_TLAS, memoryDesc);

    nri::Memory* memory = nullptr;
    NRI_ABORT_ON_FAILURE(NRI.AllocateMemory(*m_Device, nri::WHOLE_DEVICE_GROUP, memoryDesc.type, memoryDesc.size, memory));
    m_Memories.push_back(memory);

    const nri::AccelerationStructureMemoryBindingDesc memoryBindingDesc = { memory, m_TLAS };
    NRI_ABORT_ON_FAILURE(NRI.BindAccelerationStructureMemory(*m_Device, &memoryBindingDesc, 1));

    // Descriptor::Opaque_AccelerationStructure
    nri::Descriptor* descriptor;
    NRI.CreateAccelerationStructureDescriptor(*m_TLAS, 0, descriptor);
    m_Descriptors.push_back(descriptor);
}

void Sample::CreateUploadBuffer(uint64_t size, nri::Buffer*& buffer, nri::Memory*& memory)
{
    const nri::BufferDesc bufferDesc = { size, 0, (nri::BufferUsageBits)0 };
    NRI_ABORT_ON_FAILURE(NRI.CreateBuffer(*m_Device, bufferDesc, buffer));

    nri::MemoryDesc memoryDesc = {};
    NRI.GetBufferMemoryInfo(*buffer, nri::MemoryLocation::HOST_UPLOAD, memoryDesc);

    NRI_ABORT_ON_FAILURE(NRI.AllocateMemory(*m_Device, nri::WHOLE_DEVICE_GROUP, memoryDesc.type, memoryDesc.size, memory));

    const nri::BufferMemoryBindingDesc bufferMemoryBindingDesc = { memory, buffer };
    NRI_ABORT_ON_FAILURE(NRI.BindBufferMemory(*m_Device, &bufferMemoryBindingDesc, 1));
}

void Sample::CreateScratchBuffer(nri::AccelerationStructure& accelerationStructure, nri::Buffer*& buffer, nri::Memory*& memory)
{
    const uint64_t scratchBufferSize = NRI.GetAccelerationStructureBuildScratchBufferSize(accelerationStructure);

    const nri::BufferDesc bufferDesc = { scratchBufferSize, 0, nri::BufferUsageBits::RAY_TRACING_SCRATCH_BUFFER };
    NRI_ABORT_ON_FAILURE(NRI.CreateBuffer(*m_Device, bufferDesc, buffer));

    nri::MemoryDesc memoryDesc = {};
    NRI.GetBufferMemoryInfo(*buffer, nri::MemoryLocation::DEVICE, memoryDesc);

    NRI_ABORT_ON_FAILURE(NRI.AllocateMemory(*m_Device, nri::WHOLE_DEVICE_GROUP, memoryDesc.type, memoryDesc.size, memory));

    const nri::BufferMemoryBindingDesc bufferMemoryBindingDesc = { memory, buffer };
    NRI_ABORT_ON_FAILURE(NRI.BindBufferMemory(*m_Device, &bufferMemoryBindingDesc, 1));
}

void Sample::BuildBottomLevelAccelerationStructure(nri::AccelerationStructure& accelerationStructure, const nri::GeometryObject* objects, const uint32_t objectNum)
{
    nri::Buffer* scratchBuffer = nullptr;
    nri::Memory* scratchBufferMemory = nullptr;
    CreateScratchBuffer(accelerationStructure, scratchBuffer, scratchBufferMemory);

    nri::CommandAllocator* commandAllocator = nullptr;
    NRI.CreateCommandAllocator(*m_CommandQueue, nri::WHOLE_DEVICE_GROUP, commandAllocator);

    nri::CommandBuffer* commandBuffer = nullptr;
    NRI.CreateCommandBuffer(*commandAllocator, commandBuffer);

    NRI.BeginCommandBuffer(*commandBuffer, nullptr, 0);
    {
        NRI.CmdBuildBottomLevelAccelerationStructure(*commandBuffer, objectNum, objects, BUILD_FLAGS, accelerationStructure, *scratchBuffer, 0);
    }
    NRI.EndCommandBuffer(*commandBuffer);

    nri::WorkSubmissionDesc workSubmissionDesc = {};
    workSubmissionDesc.commandBuffers = &commandBuffer;
    workSubmissionDesc.commandBufferNum = 1;
    NRI.SubmitQueueWork(*m_CommandQueue, workSubmissionDesc, nullptr);

    helper::WaitIdle(NRI, *m_Device, *m_CommandQueue);

    NRI.DestroyCommandBuffer(*commandBuffer);
    NRI.DestroyCommandAllocator(*commandAllocator);
    NRI.DestroyBuffer(*scratchBuffer);
    NRI.FreeMemory(*scratchBufferMemory);
}

void Sample::BuildTopLevelAccelerationStructure(nri::CommandBuffer& commandBuffer, uint32_t bufferedFrameIndex)
{
    bool isAnimatedObjects = m_Settings.animatedObjects;
    if (m_Settings.blink)
    {
        double period = 0.0003 * m_Timer.GetTimeStamp() * (m_Settings.animationSpeed < 0.0f ? 1.0f / (1.0f + Abs(m_Settings.animationSpeed)) : (1.0f + m_Settings.animationSpeed));
        isAnimatedObjects &= WaveTriangle(period) > 0.5;
    }

    const uint64_t tlasCount = m_Scene.instances.size() - m_DefaultInstancesOffset;
    const uint64_t tlasDataSize = tlasCount * sizeof(nri::GeometryObjectInstance);
    const uint64_t tlasDataOffset = tlasDataSize * bufferedFrameIndex;
    const uint64_t instanceDataSize = tlasCount * sizeof(InstanceData);
    const uint64_t instanceDataOffset = instanceDataSize * bufferedFrameIndex;
    const uint64_t instanceCount = m_Scene.instances.size() - (m_AnimatedInstances.size() - m_Settings.animatedObjectNum * isAnimatedObjects);
    const uint64_t staticInstanceCount = m_Scene.instances.size() - m_AnimatedInstances.size();

    auto tlasData = (nri::GeometryObjectInstance*)NRI.MapBuffer(*Get(Buffer::TlasDataStaging), tlasDataOffset, tlasDataSize);
    auto instanceData = (InstanceData*)NRI.MapBuffer(*Get(Buffer::InstanceDataStaging), instanceDataOffset, instanceDataSize);

    Rand::Seed(105361);

    uint32_t instanceNum = 0;
    m_HasTransparentObjects = false;
    for (uint64_t i = m_DefaultInstancesOffset; i < instanceCount; i++)
    {
        utils::Instance& instance = m_Scene.instances[i];
        const utils::Mesh& mesh = m_Scene.meshes[instance.meshIndex];
        const utils::Material& material = m_Scene.materials[instance.materialIndex];

        if (material.IsOff()) // TODO: not an elegant way to skip "bad objects" (alpha channel is set to 0)
            continue;

        assert( instanceNum <= INSTANCE_ID_MASK );

        float4x4 mObjectToWorld = instance.rotation;
        mObjectToWorld.AddTranslation( m_Camera.GetRelative( instance.position ) );

        float4x4 mObjectToWorldPrev = instance.rotationPrev;
        mObjectToWorldPrev.AddTranslation( m_Camera.GetRelative( instance.positionPrev ) );

        float4x4 mWorldToObject = mObjectToWorld;
        mWorldToObject.Invert();

        float4x4 mWorldToWorldPrev = mObjectToWorldPrev * mWorldToObject;
        mWorldToWorldPrev.Transpose3x4();

        instance.positionPrev = instance.position;
        instance.rotationPrev = instance.rotation;

        mObjectToWorld.Transpose3x4();

        uint32_t flags = 0;
        if (material.IsEmissive())
            flags = m_Settings.emission ? FLAG_EMISSION : FLAG_OPAQUE_OR_ALPHA_OPAQUE;
        else if (m_Settings.emissiveObjects && i > staticInstanceCount && Rand::uf1() > 0.66f)
            flags = m_Settings.emission ? FLAG_FORCED_EMISSION : FLAG_OPAQUE_OR_ALPHA_OPAQUE;
        else if (material.IsTransparent())
        {
            flags = FLAG_TRANSPARENT;
            m_HasTransparentObjects = true;
        }
        else
            flags = FLAG_OPAQUE_OR_ALPHA_OPAQUE;

        uint32_t basePrimitiveId = mesh.indexOffset / 3;
        uint32_t instanceIdAndFlags = instanceNum | (flags << FLAG_FIRST_BIT);
        uint32_t averageBaseColor = material.averageBaseColor & 0x00FFFFFF;

        instanceData->mObjectToWorld0_basePrimitiveId = mObjectToWorld.col0;
        instanceData->mObjectToWorld0_basePrimitiveId.w = AsFloat(basePrimitiveId);
        instanceData->mObjectToWorld1_baseTextureIndex = mObjectToWorld.col1;
        instanceData->mObjectToWorld1_baseTextureIndex.w = AsFloat(instance.materialIndex);
        instanceData->mObjectToWorld2_averageBaseColor = mObjectToWorld.col2;
        instanceData->mObjectToWorld2_averageBaseColor.w = AsFloat(averageBaseColor);
        instanceData->mWorldToWorldPrev0 = mWorldToWorldPrev.col0;
        instanceData->mWorldToWorldPrev1 = mWorldToWorldPrev.col1;
        instanceData->mWorldToWorldPrev2 = mWorldToWorldPrev.col2;
        instanceData++;

        nri::GeometryObjectInstance& tlasInstance = *tlasData++;
        memcpy(tlasInstance.transform, mObjectToWorld.a16, sizeof(tlasInstance.transform));
        tlasInstance.instanceId = instanceIdAndFlags;
        tlasInstance.mask = flags;
        tlasInstance.shaderBindingTableLocalOffset = 0;
        tlasInstance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE | (material.IsOpaque() ? nri::TopLevelInstanceBits::FORCE_OPAQUE : nri::TopLevelInstanceBits::NONE);
        tlasInstance.accelerationStructureHandle = NRI.GetAccelerationStructureHandle(*m_BLASs[instance.meshIndex], 0);

        instanceNum++;
    }

    NRI.UnmapBuffer(*Get(Buffer::TlasDataStaging));
    NRI.UnmapBuffer(*Get(Buffer::InstanceDataStaging));

    const nri::BufferTransitionBarrierDesc transitions[] =
    {
        { Get(Buffer::InstanceData), nri::AccessBits::SHADER_RESOURCE,  nri::AccessBits::COPY_DESTINATION },
    };

    nri::TransitionBarrierDesc textureTransitionBarriers = {};
    textureTransitionBarriers.buffers = transitions;
    textureTransitionBarriers.bufferNum = helper::GetCountOf(transitions);
    NRI.CmdPipelineBarrier(commandBuffer, &textureTransitionBarriers, nullptr, nri::BarrierDependency::ALL_STAGES);

    NRI.CmdCopyBuffer(commandBuffer, *Get(Buffer::InstanceData), 0, 0, *Get(Buffer::InstanceDataStaging), 0, instanceDataOffset, instanceDataSize);
    NRI.CmdBuildTopLevelAccelerationStructure(commandBuffer, instanceNum, *Get(Buffer::TlasDataStaging), tlasDataOffset, BUILD_FLAGS, *m_TLAS, *Get(Buffer::Scratch), 0);
}

void Sample::UpdateShaderTable()
{
    uint64_t shaderTableSize = m_ShaderEntries.back();

    nri::Buffer* buffer = nullptr;
    nri::Memory* memory = nullptr;
    CreateUploadBuffer(shaderTableSize, buffer, memory);

    uint8_t* data = (uint8_t*)NRI.MapBuffer(*buffer, 0, shaderTableSize);
    {
        for (size_t i = 0; i < m_ShaderEntries.size() - 1; i++)
            NRI.WriteShaderGroupIdentifiers(*Get(Pipeline::Raytracing), (uint32_t)i, 1, data + m_ShaderEntries[i]);
    }
    NRI.UnmapBuffer(*buffer);

    nri::CommandAllocator* commandAllocator = nullptr;
    NRI.CreateCommandAllocator(*m_CommandQueue, nri::WHOLE_DEVICE_GROUP, commandAllocator);

    nri::CommandBuffer* commandBuffer = nullptr;
    NRI.CreateCommandBuffer(*commandAllocator, commandBuffer);

    NRI.BeginCommandBuffer(*commandBuffer, nullptr, 0);
    {
        NRI.CmdCopyBuffer(*commandBuffer, *Get(Buffer::ShaderTable), 0, 0, *buffer, 0, 0, shaderTableSize);
    }
    NRI.EndCommandBuffer(*commandBuffer);

    nri::WorkSubmissionDesc workSubmissionDesc = {};
    workSubmissionDesc.commandBuffers = &commandBuffer;
    workSubmissionDesc.commandBufferNum = 1;
    NRI.SubmitQueueWork(*m_CommandQueue, workSubmissionDesc, nullptr);

    helper::WaitIdle(NRI, *m_Device, *m_CommandQueue);

    NRI.DestroyCommandBuffer(*commandBuffer);
    NRI.DestroyCommandAllocator(*commandAllocator);
    NRI.DestroyBuffer(*buffer);
    NRI.FreeMemory(*memory);
}

void Sample::UpdateConstantBuffer(uint32_t frameIndex)
{
    if (m_Settings.animateSun)
    {
        const float animationSpeed = m_Settings.pauseAnimation ? 0.0f : (m_Settings.animationSpeed < 0.0f ? 1.0f / (1.0f + Abs(m_Settings.animationSpeed)) : (1.0f + m_Settings.animationSpeed));
        m_Settings.sunAzimuth = Mod(m_Settings.sunAzimuth + 0.5f * animationSpeed, 360.0f);
    }

    float3 sunDirection = GetSunDirection();

    float emissionIntensity = m_Settings.emissionIntensity * float(m_Settings.emission);

    float ambientAmount = ( m_Settings.skyAmbient + 2.0f * m_Settings.metalnessOverride * ( m_Settings.metalAmbient ? 1.0f : 0.0f ) ) * 0.01f;
    float f = Smoothstep(-0.9f, 0.05f, sunDirection.z);
    float ambient = Lerp(1000.0f, 10000.0f, Sqrt( Saturate(sunDirection.z) )) * f * ambientAmount;

    float2 screenSize = float2( (float)m_RenderResolution.x, (float)m_RenderResolution.y);
    float2 jitter = m_Settings.temporal ? m_Camera.m_ViewportJitter : 0.0f;

    const uint32_t bufferedFrameIndex = frameIndex % BUFFERED_FRAME_MAX_NUM;
    const uint64_t rangeOffset = m_Frames[bufferedFrameIndex].globalConstantBufferOffset;
    nri::Buffer* globalConstants = Get(Buffer::GlobalConstants);
    auto data = (GlobalConstantBufferData*)NRI.MapBuffer(*globalConstants, rangeOffset, sizeof(GlobalConstantBufferData));
    {
        data->gWorldToView = m_Camera.m_WorldToView;
        data->gViewToWorld = m_Camera.m_ViewToWorld;
        data->gViewToClip = m_Camera.m_ViewToClip;
        data->gWorldToClipPrev = m_Camera.m_WorldToClipPrev;
        data->gWorldToClip = m_Camera.m_WorldToClip;
        data->gCameraFrustum = m_Camera.m_Frustum;
        data->gSunDirection_gExposure = sunDirection;
        data->gSunDirection_gExposure.w = m_Settings.exposure;
        data->gWorldOrigin_gTaa = ToFloat( m_Camera.m_GlobalPosition );
        data->gWorldOrigin_gTaa.w = m_Settings.temporal ? 1.0f : 0.0f;
        data->gTrimmingParams_gEmissionIntensity = GetTrimmingParams();
        data->gTrimmingParams_gEmissionIntensity.w = emissionIntensity;
        data->gScreenSize = screenSize;
        data->gInvScreenSize = float2(1.0f, 1.0f) / screenSize;
        data->gJitter = jitter / screenSize;
        data->gAmbient = ambient * m_Settings.exposure;
        data->gSeparator = m_Settings.separator;
        data->gNearZ = (CAMERA_LEFT_HANDED ? 1.0f : -1.0f) * NEAR_Z / m_Settings.unitsToMetersMultiplier;
        data->gRoughnessOverride = m_Settings.roughnessOverride;
        data->gMetalnessOverride = m_Settings.metalnessOverride;
        data->gDiffHitDistScale = m_Settings.diffHitDistScale;
        data->gSpecHitDistScale = m_Settings.specHitDistScale;
        data->gUnitsToMetersMultiplier = m_Settings.unitsToMetersMultiplier;
        data->gIndirectDiffuse = m_Settings.indirectDiffuse ? 1.0f : 0.0f;
        data->gIndirectSpecular = m_Settings.indirectSpecular ? 1.0f : 0.0f;
        data->gTanSunAngularDiameter = Tan( DegToRad( m_Settings.sunAngularDiameter ) );
        data->gPixelAngularDiameter = DegToRad(m_Settings.camFov) / m_OutputResolution.x;
        data->gSunAngularDiameter = DegToRad(m_Settings.sunAngularDiameter);
        data->gUseMipmapping = m_Settings.mip ? 1.0f : 0.0f;
        data->gIsOrtho = m_Camera.m_IsOrtho;
        data->gDebug = m_Settings.debug;
        data->gDiffSecondBounce = m_Settings.diffSecondBounce ? 1.0f : 0.0f;
        data->gTransparent = m_HasTransparentObjects ? 1.0f : 0.0f;
        data->gOnScreen = m_Settings.onScreen;
        data->gFrameIndex = frameIndex;
        data->gForcedMaterial = m_Settings.forcedMaterial;
        data->gPrimaryFullBrdf = m_Settings.primaryFullBrdf;
        data->gIndirectFullBrdf = m_Settings.indirectFullBrdf;
        data->gUseNormalMap = m_Settings.normalMap ? 1 : 0;
        data->gWorldSpaceMotion = m_Settings.worldSpaceMotion ? 1 : 0;
        data->gUseBlueNoise = (!m_Settings.blueNoise || m_Settings.reference) ? 0 : 1;
        data->gDither = 0;
    }
    NRI.UnmapBuffer(*globalConstants);
}

void Sample::LoadScene()
{
    std::string sceneFile = utils::GetFullPath("Cubes/Cubes.obj", utils::DataFolder::SCENES);
    bool isLoaded = utils::LoadScene(sceneFile, m_Scene, false);
    NRI_ABORT_ON_FALSE(isLoaded);
    m_DefaultInstancesOffset = helper::GetCountOf(m_Scene.meshes);

    isLoaded = false;
    if (IsAutomated())
    {
        std::string sceneFile = utils::GetFullPath(m_SceneFile, utils::DataFolder::SCENES);
        isLoaded = utils::LoadScene(sceneFile, m_Scene, false);
    }
    else
    {
        bool isSelected = false;
        do
        {
            char sceneFile[1024];
            isSelected = OpenFileDialog(isLoaded ? "Add scene" : "Open scene", sceneFile, sizeof(sceneFile));
            if (isSelected)
            {
                isLoaded |= utils::LoadScene(sceneFile, m_Scene, false);
                if(isLoaded)
                    strcpy_s(m_SceneFile, sceneFile);
            }
        }
        while (isSelected);
    }
    NRI_ABORT_ON_FALSE(isLoaded);

    if (strstr(m_SceneFile, "BistroInterior"))
    {
        m_Settings.exposure = 0.006f;
        m_Settings.unitsToMetersMultiplier = 1.0f;
        m_Settings.specHitDistScale = 7.0f;
        m_Settings.sunElevation = 7.0f;
        m_Settings.skyAmbient = 1.0f;
        m_Settings.emissionIntensity = 5000.0f;
        m_Settings.emission = true;
        m_Settings.antilagIntensityThreshold = 0.08f;
    }
    else if (strstr(m_SceneFile, "BistroExterior"))
    {
        m_Settings.exposure = 0.0005f;
        m_Settings.unitsToMetersMultiplier = 1.0f;
        m_Settings.specHitDistScale = 12.0f;
        m_Settings.skyAmbient = 1.0f;
        m_Settings.emissionIntensity = 5000.0f;
        m_Settings.emission = true;
        m_Settings.antilagIntensityThreshold = 0.08f;
    }
    else if (strstr(m_SceneFile, "ShaderBalls"))
    {
        m_Settings.exposure = 0.00017f;
        m_Settings.unitsToMetersMultiplier = 1.0f;
        m_Settings.specHitDistScale = 10.0f;
        m_Settings.specSecondBounce = true;
        m_Settings.diffSecondBounce = false;
        m_Settings.skyAmbient = 10.0f;
        m_Settings.antilagIntensityThreshold = 0.1f;
    }
    else if (strstr(m_SceneFile, "ZeroDay"))
    {
        m_Settings.exposure = 0.001f;
        m_Settings.unitsToMetersMultiplier = 1.0f;
        m_Settings.emissionIntensity = 15000.0f;
        m_Settings.emission = true;
        m_Settings.roughnessOverride = 0.07f;
        m_Settings.metalnessOverride = 0.25f;
        m_Settings.specSecondBounce = true;
        m_Settings.camFov = 75.0f;
        m_Settings.indirectFullBrdf = false;
        m_Settings.primaryFullBrdf = false;
        m_Settings.animationSpeed = -0.6f;
        m_Settings.diffHitDistScale = 2.0f;
        m_Settings.specHitDistScale = 25.0f;
        m_Settings.sunElevation = -90.0f;
        m_Settings.sunAngularDiameter = 0.0f;
        m_Settings.checkerboard = false;
        m_Settings.antilagIntensityThreshold = 0.05f;
    }
}

uint32_t Sample::BuildOptimizedTransitions(const TextureState* states, uint32_t stateNum, nri::TextureTransitionBarrierDesc* transitions, uint32_t transitionMaxNum)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < stateNum; i++)
    {
        const TextureState& state = states[i];
        nri::TextureTransitionBarrierDesc& transition = GetState(state.texture);

        if (transition.nextAccess != state.nextAccess || transition.nextLayout != state.nextLayout)
        {
            assert( n < transitionMaxNum );
            transitions[n++] = nri::TextureTransition(transition, state.nextAccess, state.nextLayout);
        }
    }

    return n;
}

void Sample::RenderFrame(uint32_t frameIndex)
{
    nri::TextureTransitionBarrierDesc optimizedTransitions[32] = {};

    const uint32_t bufferedFrameIndex = frameIndex % BUFFERED_FRAME_MAX_NUM;
    const Frame& frame = m_Frames[bufferedFrameIndex];
    const uint32_t backBufferIndex = NRI.AcquireNextSwapChainTexture(*m_SwapChain, *m_BackBufferAcquireSemaphore);
    const BackBuffer* backBuffer = &m_SwapChainBuffers[backBufferIndex];
    const bool isEven = !(frameIndex & 0x1);
    nri::TransitionBarrierDesc textureTransitionBarriers = {};

    NRI.WaitForSemaphore(*m_CommandQueue, *frame.deviceSemaphore);
    NRI.ResetCommandAllocator(*frame.commandAllocator);

    UpdateConstantBuffer(frameIndex);

    // MAIN
    NRI.BeginCommandBuffer(*frame.commandBuffers[0], m_DescriptorPool, 0);
    {
        nri::CommandBuffer& commandBuffer1 = *frame.commandBuffers[0];

        // Preintegrate F (for specular) and G (for diffuse) terms (only once)
        if (frameIndex == 0)
        {
            NRI.CmdSetPipelineLayout(commandBuffer1, *GetPipelineLayout(Pipeline::IntegrateBRDF));
            NRI.CmdSetPipeline(commandBuffer1, *Get(Pipeline::IntegrateBRDF));
            NRI.CmdSetDescriptorSets(commandBuffer1, 0, 1, &Get(DescriptorSet::IntegrateBRDF0), nullptr);

            const uint32_t gridWidth = (FG_TEX_SIZE + 15) / 16;
            const uint32_t gridHeight = (FG_TEX_SIZE + 15) / 16;
            NRI.CmdDispatch(commandBuffer1, gridWidth, gridHeight, 1);

            const nri::TextureTransitionBarrierDesc transitions[] =
            {
                nri::TextureTransition(GetState(Texture::IntegrateBRDF), nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE),
            };
            textureTransitionBarriers.textures = transitions;
            textureTransitionBarriers.textureNum = helper::GetCountOf(transitions);
            NRI.CmdPipelineBarrier(commandBuffer1, &textureTransitionBarriers, nullptr, nri::BarrierDependency::ALL_STAGES);
        }

        { // TLAS
            helper::Annotation annotation(NRI, commandBuffer1, "TLAS");

            BuildTopLevelAccelerationStructure(commandBuffer1, bufferedFrameIndex);
        }

        { // Raytracing
            helper::Annotation annotation(NRI, commandBuffer1, "Raytracing");

            const nri::BufferTransitionBarrierDesc bufferTransitions[] =
            {
                { Get(Buffer::InstanceData), nri::AccessBits::COPY_DESTINATION,  nri::AccessBits::SHADER_RESOURCE },
            };

            const nri::TextureTransitionBarrierDesc transitions[] =
            {
                // Input
                nri::TextureTransition(GetState(Texture::CompositionHdr), nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE),
                // Output
                nri::TextureTransition(GetState(Texture::DirectLighting), nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL),
                nri::TextureTransition(GetState(Texture::TransparentLighting), nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL),
                nri::TextureTransition(GetState(Texture::ObjectMotion), nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL),
                nri::TextureTransition(GetState(Texture::ViewZ), nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL),
                nri::TextureTransition(GetState(Texture::Normal_Roughness), nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL),
                nri::TextureTransition(GetState(Texture::BaseColor_Metalness), nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL),
                nri::TextureTransition(GetState(Texture::Unfiltered_Shadow), nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL),
                nri::TextureTransition(GetState(Texture::Unfiltered_DiffA), nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL),
                nri::TextureTransition(GetState(Texture::Unfiltered_DiffB), nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL),
                nri::TextureTransition(GetState(Texture::Unfiltered_SpecHit), nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL),
            };

            nri::TransitionBarrierDesc transitionBarriers = {};
            transitionBarriers.textures = transitions;
            transitionBarriers.textureNum = helper::GetCountOf(transitions);
            transitionBarriers.buffers = bufferTransitions;
            transitionBarriers.bufferNum = helper::GetCountOf(bufferTransitions);
            NRI.CmdPipelineBarrier(commandBuffer1, &transitionBarriers, nullptr, nri::BarrierDependency::ALL_STAGES);

            NRI.CmdSetPipelineLayout(commandBuffer1, *GetPipelineLayout(Pipeline::Raytracing));
            NRI.CmdSetPipeline(commandBuffer1, *Get(Pipeline::Raytracing));

            const nri::DescriptorSet* descriptorSets[] = { frame.globalConstantBufferDescriptorSet, Get(DescriptorSet::Raytracing1), Get(DescriptorSet::Raytracing2) };
            NRI.CmdSetDescriptorSets(commandBuffer1, 0, helper::GetCountOf(descriptorSets), descriptorSets, nullptr);

            uint32_t raygenIndex = m_Settings.checkerboard ? 2 : 0;
            raygenIndex += m_Settings.specSecondBounce ? 1 : 0;

            nri::DispatchRaysDesc dispatchRaysDesc = {};
            dispatchRaysDesc.raygenShader = { Get(Buffer::ShaderTable), m_ShaderEntries[ShaderGroup::Raytracing00_rgen + raygenIndex], m_DeviceDesc->rayTracingShaderGroupIdentifierSize, m_DeviceDesc->rayTracingShaderGroupIdentifierSize };
            dispatchRaysDesc.missShaders = { Get(Buffer::ShaderTable), m_ShaderEntries[ShaderGroup::Main_rmiss], m_DeviceDesc->rayTracingShaderGroupIdentifierSize, m_DeviceDesc->rayTracingShaderGroupIdentifierSize };
            dispatchRaysDesc.hitShaderGroups = { Get(Buffer::ShaderTable), m_ShaderEntries[ShaderGroup::Main_rhit], m_DeviceDesc->rayTracingShaderGroupIdentifierSize, m_DeviceDesc->rayTracingShaderGroupIdentifierSize };
            dispatchRaysDesc.width = GetWindowWidth();
            dispatchRaysDesc.height = GetWindowHeight();
            dispatchRaysDesc.depth = 1;
            NRI.CmdDispatchRays(commandBuffer1, dispatchRaysDesc);
        }
    }
    NRI.EndCommandBuffer(*frame.commandBuffers[0]);

    // DENOISING
    float sunCurr = Smoothstep( -0.9f, 0.05f, Sin( DegToRad(m_Settings.sunElevation) ) );
    float sunPrev = Smoothstep( -0.9f, 0.05f, Sin( DegToRad(m_PrevSettings.sunElevation) ) );
    float resetHistoryFactor = 1.0f - Smoothstep( 0.0f, 0.2f, Abs(sunCurr - sunPrev) );

    if (m_PrevSettings.reference != m_Settings.reference)
        resetHistoryFactor = 0.0f;
    if ( (m_PrevSettings.onScreen >= 11 && m_Settings.onScreen <= 4) || (m_PrevSettings.onScreen <= 4 && m_Settings.onScreen >= 11) ) // FIXME: for mip visualization
        resetHistoryFactor = 0.0f;
    if ( m_IsActive != m_PrevIsActive )
        resetHistoryFactor = 0.0f;
    m_PrevIsActive = m_IsActive;

    NRI.BeginCommandBuffer(*frame.commandBuffers[1], nullptr, 0);
    {
        nri::CommandBuffer& commandBuffer2 = *frame.commandBuffers[1];
        float2 jitter = m_Settings.temporal ? m_Camera.m_ViewportJitter : 0.0f;
        float2 jitterPrev = m_Settings.temporal ? m_Camera.m_ViewportJitterPrev : 0.0f;
        float3 sunDirection = GetSunDirection();

        helper::Annotation annotation(NRI, commandBuffer2, "NRD denoising");

        nrd::CommonSettings commonSettings = {};
        memcpy(commonSettings.worldToViewMatrix, &m_Camera.m_WorldToView, sizeof(m_Camera.m_WorldToView));
        memcpy(commonSettings.worldToViewMatrixPrev, &m_Camera.m_WorldToViewPrev, sizeof(m_Camera.m_WorldToViewPrev));
        memcpy(commonSettings.viewToClipMatrix, &m_Camera.m_ViewToClip, sizeof(m_Camera.m_ViewToClip));
        memcpy(commonSettings.viewToClipMatrixPrev, &m_Camera.m_ViewToClipPrev, sizeof(m_Camera.m_ViewToClipPrev));
        commonSettings.metersToUnitsMultiplier = 1.0f / m_Settings.unitsToMetersMultiplier;
        commonSettings.denoisingRange = m_Scene.aabb.GetRadius() * 4.0f;
        commonSettings.xMotionVectorScale = m_Settings.worldSpaceMotion ? 1.0f : 1.0f / GetWindowWidth();
        commonSettings.yzMotionVectorScale = m_Settings.worldSpaceMotion ? 1.0f : 1.0f / GetWindowHeight();
        commonSettings.xJitter = jitter.x;
        commonSettings.yJitter = jitter.y;
        commonSettings.debug = m_Settings.debug;
        commonSettings.frameIndex = frameIndex;
        commonSettings.worldSpaceMotion = m_Settings.worldSpaceMotion;
        commonSettings.forceReferenceAccumulation = m_Settings.reference;

        // TODO: replace with "a portion of the average intensity of the previous frame"
        float threshold0 = m_Settings.antilagIntensityThreshold * m_Settings.emissionIntensity * m_Settings.exposure * 0.01f;
        float b = Smoothstep(-0.9f, 0.05f, sunDirection.z);
        threshold0 += 0.1f * b * b;
        threshold0 *= 1920.0f / GetWindowWidth(); // Additionally it depends on the resolution (higher resolution = better samples)

        nrd::AntilagSettings antilagSettings = {};
        antilagSettings.enable = m_Settings.antilag;
        antilagSettings.intensityThresholdMin = threshold0;
        antilagSettings.intensityThresholdMax = 3.0f * threshold0;

        nrd::DiffuseSettings diffuseSettings = {};
        diffuseSettings.hitDistanceParameters = { m_Settings.diffHitDistScale, 0.1f, 0.0f, 0.0f }; // see HIT_DISTANCE_LINEAR_SCALE
        diffuseSettings.antilagSettings = antilagSettings;
        diffuseSettings.maxAccumulatedFrameNum = uint32_t(m_Settings.diffMaxHistoryFrameNum * resetHistoryFactor + 0.5f);
        diffuseSettings.disocclusionThreshold = m_Settings.disocclusionThreshold * 0.01f;
        diffuseSettings.denoisingRadius = m_Settings.diffDenoisingRadius;
        diffuseSettings.maxAdaptiveRadiusScale = m_Settings.diffAdaptiveRadiusScale;
        diffuseSettings.checkerboard = m_Settings.checkerboard;
        m_NRD.SetMethodSettings(nrd::Method::DIFFUSE, &diffuseSettings);

        const float3 trimmingParams = GetTrimmingParams();

        nrd::SpecularSettings specularSettings = {};
        specularSettings.hitDistanceParameters = { m_Settings.specHitDistScale, 0.1f, 0.0f, 0.0f }; // see HIT_DISTANCE_LINEAR_SCALE
        specularSettings.lobeTrimmingParameters = { trimmingParams.x, trimmingParams.y, trimmingParams.z };
        specularSettings.antilagSettings = antilagSettings;
        specularSettings.maxAccumulatedFrameNum = uint32_t(m_Settings.specMaxHistoryFrameNum * resetHistoryFactor + 0.5f);
        specularSettings.disocclusionThreshold = m_Settings.disocclusionThreshold * 0.01f;
        specularSettings.denoisingRadius = m_Settings.specDenoisingRadius;
        specularSettings.minAdaptiveRadiusScale = m_Settings.specAdaptiveRadiusScale;
        specularSettings.anisotropicFiltering = m_Settings.specularAnisotropicFiltering;
        specularSettings.checkerboard = m_Settings.checkerboard;
        m_NRD.SetMethodSettings(nrd::Method::SPECULAR, &specularSettings);

        nrd::ShadowSettings shadowSettings = {};
        shadowSettings.directionToLightSource[0] = sunDirection.x;
        shadowSettings.directionToLightSource[1] = sunDirection.y;
        shadowSettings.directionToLightSource[2] = sunDirection.z;
        shadowSettings.lightSourceAngularDiameter = m_Settings.sunAngularDiameter;
        m_NRD.SetMethodSettings(nrd::Method::SHADOW, &shadowSettings);

        NrdUserPool userPool =
        {{
            // IN_MOTION_VECTOR
            {Get(Texture::ObjectMotion), &GetState(Texture::ObjectMotion), nri::Format::RGBA16_SFLOAT},

            // IN_NORMAL_ROUGHNESS
            {Get(Texture::Normal_Roughness), &GetState(Texture::Normal_Roughness), nri::Format::RGBA8_UNORM},

            // IN_VIEWZ
            {Get(Texture::ViewZ), &GetState(Texture::ViewZ), nri::Format::R32_SFLOAT},

            // IN_SHADOW
            {Get(Texture::Unfiltered_Shadow), &GetState(Texture::Unfiltered_Shadow), nri::Format::RG16_SFLOAT},

            // IN_DIFF_A
            {Get(Texture::Unfiltered_DiffA), &GetState(Texture::Unfiltered_DiffA), nri::Format::RGBA16_SFLOAT},

            // IN_DIFF_B
            {Get(Texture::Unfiltered_DiffB), &GetState(Texture::Unfiltered_DiffB), nri::Format::RGBA16_SFLOAT},

            // IN_SPEC_HIT
            {Get(Texture::Unfiltered_SpecHit), &GetState(Texture::Unfiltered_SpecHit), nri::Format::RGBA16_SFLOAT},

            // OUT_SHADOW
            {Get(Texture::Shadow), &GetState(Texture::Shadow), nri::Format::R8_UNORM},

            // OUT_DIFF_HIT
            {Get(Texture::DiffHit), &GetState(Texture::DiffHit), nri::Format::RGBA16_SFLOAT},

            // OUT_SPEC_HIT
            {Get(Texture::SpecHit), &GetState(Texture::SpecHit), nri::Format::RGBA16_SFLOAT},
        }};

        m_NRD.Denoise(commandBuffer2, commonSettings, userPool);
    }
    NRI.EndCommandBuffer(*frame.commandBuffers[1]);

    // COMPOSITION
    NRI.BeginCommandBuffer(*frame.commandBuffers[2], m_DescriptorPool, 0);
    {
        nri::CommandBuffer& commandBuffer3 = *frame.commandBuffers[2];

        { // Composition
            helper::Annotation annotation(NRI, commandBuffer3, "Composition");

            const TextureState transitions[] =
            {
                // Input
                {Texture::DirectLighting, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::TransparentLighting, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::Normal_Roughness, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::BaseColor_Metalness, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::Shadow, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::DiffHit, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::SpecHit, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::Unfiltered_Shadow, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::Unfiltered_DiffA, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::Unfiltered_DiffB, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::Unfiltered_SpecHit, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                // Output
                {Texture::Composition, nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL},
                {Texture::CompositionHdr, nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL},
            };
            textureTransitionBarriers.textures = optimizedTransitions;
            textureTransitionBarriers.textureNum = BuildOptimizedTransitions(transitions, helper::GetCountOf(transitions), optimizedTransitions, helper::GetCountOf(optimizedTransitions));
            NRI.CmdPipelineBarrier(commandBuffer3, &textureTransitionBarriers, nullptr, nri::BarrierDependency::ALL_STAGES);

            NRI.CmdSetPipelineLayout(commandBuffer3, *GetPipelineLayout(Pipeline::Composition));
            NRI.CmdSetPipeline(commandBuffer3, *Get(Pipeline::Composition));

            const nri::DescriptorSet* descriptorSets[] = { frame.globalConstantBufferDescriptorSet, Get(DescriptorSet::Composition1) };
            NRI.CmdSetDescriptorSets(commandBuffer3, 0, helper::GetCountOf(descriptorSets), descriptorSets, nullptr);

            const uint32_t gridWidth = (m_RenderResolution.x + 15) / 16;
            const uint32_t gridHeight = (m_RenderResolution.y + 15) / 16;
            NRI.CmdDispatch(commandBuffer3, gridWidth, gridHeight, 1);
        }

        { // Temporal
            helper::Annotation annotation(NRI, commandBuffer3, "Temporal");

            const TextureState transitions[] =
            {
                // Input
                {Texture::ViewZ, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::ObjectMotion, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {Texture::Composition, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                {isEven ? Texture::TaaHistoryPrev : Texture::TaaHistory, nri::AccessBits::SHADER_RESOURCE, nri::TextureLayout::SHADER_RESOURCE},
                // Output
                {isEven ? Texture::TaaHistory : Texture::TaaHistoryPrev, nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL},
                {Texture::Final, nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::TextureLayout::GENERAL},
            };
            textureTransitionBarriers.textures = optimizedTransitions;
            textureTransitionBarriers.textureNum = BuildOptimizedTransitions(transitions, helper::GetCountOf(transitions), optimizedTransitions, helper::GetCountOf(optimizedTransitions));
            NRI.CmdPipelineBarrier(commandBuffer3, &textureTransitionBarriers, nullptr, nri::BarrierDependency::ALL_STAGES);

            NRI.CmdSetPipelineLayout(commandBuffer3, *GetPipelineLayout(Pipeline::Temporal));
            NRI.CmdSetPipeline(commandBuffer3, *Get(Pipeline::Temporal));

            const nri::DescriptorSet* descriptorSets[] =
            {
                frame.globalConstantBufferDescriptorSet,
                Get(isEven ? DescriptorSet::Temporal1a : DescriptorSet::Temporal1b)
            };
            NRI.CmdSetDescriptorSets(commandBuffer3, 0, helper::GetCountOf(descriptorSets), descriptorSets, nullptr);

            const uint32_t gridWidth = (m_RenderResolution.x + 15) / 16;
            const uint32_t gridHeight = (m_RenderResolution.y + 15) / 16;
            NRI.CmdDispatch(commandBuffer3, gridWidth, gridHeight, 1);
        }

        { // Copy
            const nri::TextureTransitionBarrierDesc transitions[] =
            {
                nri::TextureTransition(GetState(Texture::Final), nri::AccessBits::COPY_SOURCE, nri::TextureLayout::GENERAL),
                nri::TextureTransition(backBuffer->texture, nri::AccessBits::UNKNOWN, nri::AccessBits::COPY_DESTINATION, nri::TextureLayout::UNKNOWN, nri::TextureLayout::GENERAL),
            };
            textureTransitionBarriers.textures = transitions;
            textureTransitionBarriers.textureNum = helper::GetCountOf(transitions);
            NRI.CmdPipelineBarrier(commandBuffer3, &textureTransitionBarriers, nullptr, nri::BarrierDependency::ALL_STAGES);

            NRI.CmdCopyTexture(commandBuffer3, *backBuffer->texture, 0, nullptr, *Get(Texture::Final), 0, nullptr);
        }

        { // UI
            const nri::TextureTransitionBarrierDesc beforeTransitions = nri::TextureTransition(backBuffer->texture, nri::AccessBits::COPY_DESTINATION, nri::AccessBits::COLOR_ATTACHMENT, nri::TextureLayout::GENERAL, nri::TextureLayout::COLOR_ATTACHMENT);
            textureTransitionBarriers.textures = &beforeTransitions;
            textureTransitionBarriers.textureNum = 1;
            NRI.CmdPipelineBarrier(commandBuffer3, &textureTransitionBarriers, nullptr, nri::BarrierDependency::ALL_STAGES);

            NRI.CmdBeginRenderPass(commandBuffer3, *backBuffer->frameBufferUI, nri::FramebufferBindFlag::SKIP_CLEAR);
            m_UserInterface.Render(commandBuffer3);
            NRI.CmdEndRenderPass(commandBuffer3);

            const nri::TextureTransitionBarrierDesc afterTransitions = nri::TextureTransition(backBuffer->texture, nri::AccessBits::COLOR_ATTACHMENT, nri::AccessBits::UNKNOWN, nri::TextureLayout::COLOR_ATTACHMENT, nri::TextureLayout::PRESENT);
            textureTransitionBarriers.textures = &afterTransitions;
            textureTransitionBarriers.textureNum = 1;
            NRI.CmdPipelineBarrier(commandBuffer3, &textureTransitionBarriers, nullptr, nri::BarrierDependency::ALL_STAGES);
        }
    }
    NRI.EndCommandBuffer(*frame.commandBuffers[2]);

    nri::WorkSubmissionDesc workSubmissionDesc = {};
    workSubmissionDesc.wait = &m_BackBufferAcquireSemaphore;
    workSubmissionDesc.waitNum = 1;
    workSubmissionDesc.commandBuffers = frame.commandBuffers.data();
    workSubmissionDesc.commandBufferNum = (uint32_t)frame.commandBuffers.size();
    workSubmissionDesc.signal = &m_BackBufferReleaseSemaphore;
    workSubmissionDesc.signalNum = 1;
    NRI.SubmitQueueWork(*m_CommandQueue, workSubmissionDesc, frame.deviceSemaphore);

    NRI.SwapChainPresent(*m_SwapChain, *m_BackBufferReleaseSemaphore);

    m_Timer.UpdateElapsedTimeSinceLastSave();
    m_Timer.SaveCurrentTime();
}

#include "NRDIntegration.hpp"

SAMPLE_MAIN(Sample, 0);
