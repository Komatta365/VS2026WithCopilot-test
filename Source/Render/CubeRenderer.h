#pragma once

#include "DirectXMain.h"
#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <stdexcept>
#include <chrono>
#include <cstring>
#include <array>
#include <cmath>
#include "../Game/GameLoop.h"

namespace SceneRendererInternal {
using namespace DirectX;

constexpr UINT kShadowMapSize = 2048;
constexpr UINT kMaxObjects = 200;
constexpr float kGroundHeight = -1.5f;

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT3 color;
    XMFLOAT2 uv;
};

struct MeshBuffers {
    ComPtr<ID3D12Resource> vertex;
    ComPtr<ID3D12Resource> index;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    UINT indexCount = 0;
    UINT objectIndex = 0;
};

struct SceneConstants {
    XMFLOAT4X4 viewProj;
    XMFLOAT4X4 lightViewProj;
    XMFLOAT3 lightDir;
    float shadowBias;
    XMFLOAT3 cameraPos;
    float padding;
};

struct ObjectConstants {
    XMFLOAT4X4 world;
};

struct RendererState {
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> litPipelineState;
    ComPtr<ID3D12PipelineState> shadowPipelineState;

    MeshBuffers cube;
    MeshBuffers ground;
    MeshBuffers ship;

    ComPtr<ID3D12Resource> sceneCB;
    ComPtr<ID3D12Resource> objectCB;
    uint8_t* sceneCbMapped = nullptr;
    uint8_t* objectCbMapped = nullptr;
    UINT objectCbStride = 0;

    ComPtr<ID3D12Resource> shadowMap;
    ComPtr<ID3D12DescriptorHeap> shadowDsvHeap;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12Resource> buildingTexture;
    ComPtr<ID3D12Resource> buildingTextureUpload;
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE buildingSrvGpu{};
    UINT srvDescriptorSize = 0;
    D3D12_RESOURCE_STATES shadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_VIEWPORT shadowViewport{};
    D3D12_RECT shadowScissor{};

    std::chrono::steady_clock::time_point lastAnimationTime = std::chrono::steady_clock::now();
    float cubeRotation = 0.0f;
};

inline RendererState g_state{};

constexpr UINT Align256(UINT value)
{
    return (value + 255u) & ~255u;
}

inline void ThrowIfFailedCube(HRESULT hr, const char* message)
{
    if (FAILED(hr)) {
        throw std::runtime_error(message);
    }
}

inline void TransitionResource(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

inline void CreateUploadBuffer(ID3D12Device* device, UINT64 size, ComPtr<ID3D12Resource>& resource)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.SampleDesc = { 1, 0 };
    ThrowIfFailedCube(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)), "Create upload buffer failed");
}

inline void UploadData(ID3D12Resource* resource, const void* data, size_t size)
{
    void* mapped = nullptr;
    D3D12_RANGE range{ 0, 0 };
    resource->Map(0, &range, &mapped);
    std::memcpy(mapped, data, size);
    resource->Unmap(0, nullptr);
}

inline void CreateMesh(ID3D12Device* device, MeshBuffers& mesh, const Vertex* vertices, UINT vertexCount, const uint16_t* indices, UINT indexCount, UINT objectIndex)
{
    mesh.indexCount = indexCount;
    mesh.objectIndex = objectIndex;

    const UINT vbSize = static_cast<UINT>(vertexCount * sizeof(Vertex));
    CreateUploadBuffer(device, vbSize, mesh.vertex);
    UploadData(mesh.vertex.Get(), vertices, vbSize);
    mesh.vbv.BufferLocation = mesh.vertex->GetGPUVirtualAddress();
    mesh.vbv.StrideInBytes = sizeof(Vertex);
    mesh.vbv.SizeInBytes = vbSize;

    const UINT ibSize = static_cast<UINT>(indexCount * sizeof(uint16_t));
    CreateUploadBuffer(device, ibSize, mesh.index);
    UploadData(mesh.index.Get(), indices, ibSize);
    mesh.ibv.BufferLocation = mesh.index->GetGPUVirtualAddress();
    mesh.ibv.SizeInBytes = ibSize;
    mesh.ibv.Format = DXGI_FORMAT_R16_UINT;
}

inline void WriteObjectMatrix(uint8_t* base, UINT stride, UINT index, const XMMATRIX& matrix)
{
    ObjectConstants constants{};
    XMStoreFloat4x4(&constants.world, XMMatrixTranspose(matrix));
    std::memcpy(base + static_cast<size_t>(stride) * index, &constants, sizeof(constants));
}

inline void WriteSceneConstants(RendererState& state, const XMMATRIX& viewProj, const XMMATRIX& lightViewProj, const XMFLOAT3& lightDir, const XMFLOAT3& cameraPos)
{
    SceneConstants constants{};
    XMStoreFloat4x4(&constants.viewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&constants.lightViewProj, XMMatrixTranspose(lightViewProj));
    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&lightDir));
    XMStoreFloat3(&constants.lightDir, dir);
    constants.shadowBias = 0.0005f;
    constants.cameraPos = cameraPos;
    std::memcpy(state.sceneCbMapped, &constants, sizeof(constants));
}

inline void RecordDraw(ID3D12GraphicsCommandList* list, RendererState& state, const MeshBuffers& mesh, UINT objectIndex)
{
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &mesh.vbv);
    list->IASetIndexBuffer(&mesh.ibv);
    const D3D12_GPU_VIRTUAL_ADDRESS objAddress = state.objectCB->GetGPUVirtualAddress() + static_cast<UINT64>(state.objectCbStride) * objectIndex;
    list->SetGraphicsRootConstantBufferView(0, objAddress);
    list->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
}

} // namespace SceneRendererInternal

inline void InitializeCubeRenderer(D3D12Context& ctx)
{
    using namespace SceneRendererInternal;
    auto& state = g_state;
    state.shadowViewport = { 0.0f, 0.0f, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0.0f, 1.0f };
    state.shadowScissor = { 0, 0, static_cast<LONG>(kShadowMapSize), static_cast<LONG>(kShadowMapSize) };

    // Shadow map resources
    D3D12_RESOURCE_DESC shadowDesc{};
    shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadowDesc.Width = kShadowMapSize;
    shadowDesc.Height = kShadowMapSize;
    shadowDesc.DepthOrArraySize = 1;
    shadowDesc.MipLevels = 1;
    shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    shadowDesc.SampleDesc = { 1, 0 };
    shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    ThrowIfFailedCube(ctx.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &shadowDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&state.shadowMap)), "Create shadow map failed");

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailedCube(ctx.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&state.shadowDsvHeap)), "Create shadow DSV heap failed");
    state.shadowDsv = state.shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    ctx.device->CreateDepthStencilView(state.shadowMap.Get(), &dsvDesc, state.shadowDsv);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = 2; // ShadowMap + BuildingTexture
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailedCube(ctx.device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&state.srvHeap)), "Create shadow SRV heap failed");
    state.srvDescriptorSize = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const auto srvCpu = state.srvHeap->GetCPUDescriptorHandleForHeapStart();
    ctx.device->CreateShaderResourceView(state.shadowMap.Get(), &srvDesc, srvCpu);
    state.shadowSrvGpu = state.srvHeap->GetGPUDescriptorHandleForHeapStart();

    // Create Building Texture SRV at offset 1 (descriptor for building texture, resource will be created later)
    D3D12_CPU_DESCRIPTOR_HANDLE texSrvCpu = srvCpu;
    texSrvCpu.ptr += state.srvDescriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC texSrvDesc{};
    texSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    texSrvDesc.Texture2D.MipLevels = 1;
    // SRV will be created after buildingTexture is initialized below
    state.buildingSrvGpu = state.shadowSrvGpu;
    state.buildingSrvGpu.ptr += state.srvDescriptorSize;

    // Root signature
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE texRange{};
    texRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texRange.NumDescriptors = 1;
    texRange.BaseShaderRegister = 1; // t1
    texRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 4> rootParams{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &texRange;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.ShaderRegister = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC texSampler{};
    texSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    texSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    texSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    texSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    texSampler.ShaderRegister = 1; // s1
    texSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[] = { shadowSampler, texSampler };

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(rootParams.size());
    rsDesc.pParameters = rootParams.data();
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob;
    ComPtr<ID3DBlob> rsError;
    ThrowIfFailedCube(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError), "Serialize scene root signature failed");
    ThrowIfFailedCube(ctx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&state.rootSignature)), "Create scene root signature failed");

    // Shaders
    const char* litVs = R"(
        cbuffer ObjectCB : register(b0) { float4x4 world; };
        cbuffer SceneCB : register(b1) { float4x4 viewProj; float4x4 lightViewProj; float3 lightDir; float shadowBias; float3 cameraPos; float padding; };
        struct VSInput { float3 pos : POSITION; float3 normal : NORMAL; float3 col : COLOR; float2 uv : TEXCOORD; };
        struct PSInput { float4 pos : SV_POSITION; float3 normal : NORMAL; float3 color : COLOR; float4 lightClip : TEXCOORD0; float3 worldPos : TEXCOORD1; float2 uv : TEXCOORD2; };
        PSInput main(VSInput input) {
            PSInput o;
            float4 worldPos = mul(float4(input.pos, 1.0f), world);
            float3 worldNormal = mul((float3x3)world, input.normal);
            float4 shadowPos = worldPos;
            shadowPos.xyz += normalize(worldNormal) * 0.01f;
            o.pos = mul(worldPos, viewProj);
            o.lightClip = mul(shadowPos, lightViewProj);
            o.normal = worldNormal;
            o.color = input.col;
            o.worldPos = worldPos.xyz;
            o.uv = input.uv;
            return o;
        }
    )";

    const char* litPs = R"(
        Texture2D shadowMap : register(t0);
        Texture2D mainTex : register(t1);
        SamplerComparisonState shadowSampler : register(s0);
        SamplerState texSampler : register(s1);
        cbuffer SceneCB : register(b1) { float4x4 viewProj; float4x4 lightViewProj; float3 lightDir; float shadowBias; float3 cameraPos; float padding; };
        struct PSInput { float4 pos : SV_POSITION; float3 normal : NORMAL; float3 color : COLOR; float4 lightClip : TEXCOORD0; float3 worldPos : TEXCOORD1; float2 uv : TEXCOORD2; };
        float ShadowFactor(float4 lightClip, float3 normal, float3 lightVec) {
            float3 projCoords = lightClip.xyz / lightClip.w;
            float2 shadowUV = projCoords.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
            if (shadowUV.x < 0.0f || shadowUV.x > 1.0f || shadowUV.y < 0.0f || shadowUV.y > 1.0f || projCoords.z < 0.0f || projCoords.z > 1.0f) {
                return 1.0f;
            }
            float ndotl = saturate(dot(normalize(normal), normalize(-lightVec)));
            float dynamicBias = max(shadowBias * 0.5f, (1.0f - ndotl) * shadowBias * 4.0f);
            return shadowMap.SampleCmpLevelZero(shadowSampler, shadowUV, projCoords.z - dynamicBias);
        }
        float4 main(PSInput input) : SV_Target {
            float3 n = normalize(input.normal);
            float3 l = normalize(-lightDir);
            float diff = saturate(dot(n, l));
            float shadow = ShadowFactor(input.lightClip, n, lightDir);
            float ambient = 0.1f;
            float4 texColor = mainTex.Sample(texSampler, input.uv);
            // Use texture alpha to mix with vertex color, or just multiply. 
            // For buildings, we want the texture. For others (no texture bound?), we might have issues.
            // Since we bind the same texture for everything for now, let's just multiply.
            // But wait, ship and ground use vertex colors primarily.
            // Let's mix: if texture is mostly grey/white, vertex color tints it.
            // Our procedural texture is yellow windows and dark walls.
            // Let's just multiply.
            float3 lighting = input.color * texColor.rgb * (ambient + diff * shadow);
            return float4(lighting, 1.0f);
        }
    )";

    const char* shadowVs = R"(
        cbuffer ObjectCB : register(b0) { float4x4 world; };
        cbuffer SceneCB : register(b1) { float4x4 viewProj; float4x4 lightViewProj; float3 lightDir; float shadowBias; float3 cameraPos; float padding; };
        struct VSInput { float3 pos : POSITION; float3 normal : NORMAL; float3 col : COLOR; float2 uv : TEXCOORD; };
        struct VSOutput { float4 pos : SV_POSITION; };
        VSOutput main(VSInput input) {
            VSOutput o;
            float4 worldPos = mul(float4(input.pos, 1.0f), world);
            o.pos = mul(worldPos, lightViewProj);
            return o;
        }
    )";

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> shadowVsBlob;
    ComPtr<ID3DBlob> compileErr;
    ThrowIfFailedCube(D3DCompile(litVs, std::strlen(litVs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &compileErr), "Lit VS compile failed");
    ThrowIfFailedCube(D3DCompile(litPs, std::strlen(litPs), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &compileErr), "Lit PS compile failed");
    ThrowIfFailedCube(D3DCompile(shadowVs, std::strlen(shadowVs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &shadowVsBlob, &compileErr), "Shadow VS compile failed");

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_BACK;
    rast.FrontCounterClockwise = FALSE;
    rast.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = state.rootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState = blend;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = rast;
    psoDesc.DepthStencilState = depth;
    psoDesc.InputLayout = { layout, _countof(layout) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    ThrowIfFailedCube(ctx.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&state.litPipelineState)), "Create lit PSO failed");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowDescPso = psoDesc;
    shadowDescPso.VS = { shadowVsBlob->GetBufferPointer(), shadowVsBlob->GetBufferSize() };
    shadowDescPso.PS = { nullptr, 0 };
    shadowDescPso.InputLayout = { layout, _countof(layout) };
    shadowDescPso.BlendState = D3D12_BLEND_DESC{};
    shadowDescPso.DepthStencilState = depth;
    shadowDescPso.NumRenderTargets = 0;
    shadowDescPso.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    shadowDescPso.SampleDesc.Count = 1;
    shadowDescPso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    ThrowIfFailedCube(ctx.device->CreateGraphicsPipelineState(&shadowDescPso, IID_PPV_ARGS(&state.shadowPipelineState)), "Create shadow PSO failed");

    // Mesh data
    const std::array<Vertex, 24> cubeVertices = {
        Vertex{ { -1, -1, -1 }, { 0, 0, -1 }, { 1, 0, 0 }, { 0, 1 } },
        Vertex{ { -1, 1, -1 }, { 0, 0, -1 }, { 1, 0, 0 }, { 0, 0 } },
        Vertex{ { 1, 1, -1 }, { 0, 0, -1 }, { 1, 0, 0 }, { 1, 0 } },
        Vertex{ { 1, -1, -1 }, { 0, 0, -1 }, { 1, 0, 0 }, { 1, 1 } },
        Vertex{ { -1, -1, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 1, 1 } },
        Vertex{ { 1, -1, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1 } },
        Vertex{ { 1, 1, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0 } },
        Vertex{ { -1, 1, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 1, 0 } },
        Vertex{ { -1, -1, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 1, 1 } },
        Vertex{ { -1, -1, 1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, 1 } },
        Vertex{ { -1, 1, 1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0 } },
        Vertex{ { -1, 1, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 1, 0 } },
        Vertex{ { 1, -1, -1 }, { 1, 0, 0 }, { 0, 1, 1 }, { 0, 1 } },
        Vertex{ { 1, 1, -1 }, { 1, 0, 0 }, { 0, 1, 1 }, { 0, 0 } },
        Vertex{ { 1, 1, 1 }, { 1, 0, 0 }, { 0, 1, 1 }, { 1, 0 } },
        Vertex{ { 1, -1, 1 }, { 1, 0, 0 }, { 0, 1, 1 }, { 1, 1 } },
        Vertex{ { -1, 1, -1 }, { 0, 1, 0 }, { 1, 1, 0 }, { 0, 1 } },
        Vertex{ { -1, 1, 1 }, { 0, 1, 0 }, { 1, 1, 0 }, { 0, 0 } },
        Vertex{ { 1, 1, 1 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0 } },
        Vertex{ { 1, 1, -1 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1 } },
        Vertex{ { -1, -1, -1 }, { 0, -1, 0 }, { 1, 0, 1 }, { 0, 0 } },
        Vertex{ { 1, -1, -1 }, { 0, -1, 0 }, { 1, 0, 1 }, { 1, 0 } },
        Vertex{ { 1, -1, 1 }, { 0, -1, 0 }, { 1, 0, 1 }, { 1, 1 } },
        Vertex{ { -1, -1, 1 }, { 0, -1, 0 }, { 1, 0, 1 }, { 0, 1 } },
    };

    const std::array<uint16_t, 36> cubeIndices = {
        0, 1, 2, 0, 2, 3,
        4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19,
        20, 21, 22, 20, 22, 23
    };

    CreateMesh(ctx.device.Get(), state.cube, cubeVertices.data(), static_cast<UINT>(cubeVertices.size()), cubeIndices.data(), static_cast<UINT>(cubeIndices.size()), 0);

    const float groundSize = 20.0f;
    const std::array<Vertex, 4> groundVertices = {
        Vertex{ { -groundSize, 0.0f, -groundSize }, { 0, 1, 0 }, { 0.3f, 0.7f, 0.3f }, { 0, 0 } },
        Vertex{ { groundSize, 0.0f, -groundSize }, { 0, 1, 0 }, { 0.3f, 0.7f, 0.3f }, { 10, 0 } },
        Vertex{ { groundSize, 0.0f, groundSize }, { 0, 1, 0 }, { 0.3f, 0.7f, 0.3f }, { 10, 10 } },
        Vertex{ { -groundSize, 0.0f, groundSize }, { 0, 1, 0 }, { 0.3f, 0.7f, 0.3f }, { 0, 10 } },
    };

    const std::array<uint16_t, 6> groundIndices = { 0, 2, 1, 0, 3, 2 };
    CreateMesh(ctx.device.Get(), state.ground, groundVertices.data(), static_cast<UINT>(groundVertices.size()), groundIndices.data(), static_cast<UINT>(groundIndices.size()), 0);

    // Spaceship mesh (simple arrow shape)
    const std::array<Vertex, 18> shipVertices = {
        // Body
        Vertex{ { 0.0f, 0.0f, 2.0f }, { 0.0f, 1.0f, 0.0f }, { 0.8f, 0.8f, 1.0f }, { 0.5f, 0.0f } }, // Nose
        Vertex{ { -1.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.4f, 0.4f, 0.8f }, { 0.0f, 1.0f } }, // Left Wing
        Vertex{ { 1.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.4f, 0.4f, 0.8f }, { 1.0f, 1.0f } }, // Right Wing
        Vertex{ { 0.0f, 0.5f, -0.5f }, { 0.0f, 1.0f, 0.0f }, { 0.6f, 0.6f, 0.9f }, { 0.5f, 0.5f } }, // Cockpit
        
        // Bottom
        Vertex{ { 0.0f, 0.0f, 2.0f }, { 0.0f, -1.0f, 0.0f }, { 0.3f, 0.3f, 0.5f }, { 0.5f, 0.0f } },
        Vertex{ { 1.0f, 0.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, { 0.3f, 0.3f, 0.5f }, { 1.0f, 1.0f } },
        Vertex{ { -1.0f, 0.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, { 0.3f, 0.3f, 0.5f }, { 0.0f, 1.0f } },

        // Rear
        Vertex{ { -1.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.2f, 0.2f, 0.4f }, { 0.0f, 1.0f } },
        Vertex{ { 1.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.2f, 0.2f, 0.4f }, { 1.0f, 1.0f } },
        Vertex{ { 0.0f, 0.5f, -0.5f }, { 0.0f, 0.0f, -1.0f }, { 0.2f, 0.2f, 0.4f }, { 0.5f, 0.5f } },
    };

    const std::array<uint16_t, 12> shipIndices = {
        0, 2, 3, // Top Right
        0, 3, 1, // Top Left
        0, 1, 2, // Bottom
        1, 3, 2  // Rear
    };
    // Note: Reusing indices for simplicity, but normals might be slightly off for sharp edges without duplicating vertices.
    // For a simple low-poly look, this is acceptable.
    // Actually, let's use the defined vertices properly.
    // Top: 0-2-3, 0-3-1
    // Bottom: 4-5-6 (0,2,1 copies)
    // Rear: 7-9-8 (1,3,2 copies)
    
    // Correct indices for the vertex array above:
    const std::array<uint16_t, 12> shipIndicesCorrect = {
        0, 2, 3, // Top Right
        0, 3, 1, // Top Left
        4, 5, 6, // Bottom
        7, 9, 8  // Rear
    };

    CreateMesh(ctx.device.Get(), state.ship, shipVertices.data(), static_cast<UINT>(shipVertices.size()), shipIndicesCorrect.data(), static_cast<UINT>(shipIndicesCorrect.size()), 0);

    // Create procedural texture (window pattern)
    const UINT texWidth = 256;
    const UINT texHeight = 256;
    std::vector<UINT> texData(texWidth * texHeight);
    for (UINT y = 0; y < texHeight; ++y) {
        for (UINT x = 0; x < texWidth; ++x) {
            bool isWindow = ((x % 32) < 20) && ((y % 64) < 40);
            if (isWindow) {
                // Yellowish window color
                texData[y * texWidth + x] = 0xFF80C0FF; // ABGR (A=FF, B=80, G=C0, R=FF)
            } else {
                // Dark wall color
                texData[y * texWidth + x] = 0xFF404040;
            }
        }
    }

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = texWidth;
    texDesc.Height = texHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc = { 1, 0 };
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    // Create default-heap texture for buildings
    D3D12_HEAP_PROPERTIES texHeapProps{};
    texHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    ThrowIfFailedCube(ctx.device->CreateCommittedResource(
        &texHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&state.buildingTexture)), "Create texture resource failed");

    // Upload texture data manually using copyable footprints
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    D3D12_RESOURCE_DESC texDescCopy = texDesc;
    ctx.device->GetCopyableFootprints(&texDescCopy, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    CreateUploadBuffer(ctx.device.Get(), totalBytes, state.buildingTextureUpload);

    void* mappedData = nullptr;
    state.buildingTextureUpload->Map(0, nullptr, &mappedData);
    for (UINT y = 0; y < texHeight; ++y) {
        UINT* destRow = reinterpret_cast<UINT*>(static_cast<uint8_t*>(mappedData) + footprint.Offset + static_cast<UINT64>(y) * footprint.Footprint.RowPitch);
        const UINT* srcRow = texData.data() + static_cast<size_t>(y) * texWidth;
        std::memcpy(destRow, srcRow, static_cast<size_t>(texWidth) * sizeof(UINT));
    }
    state.buildingTextureUpload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = state.buildingTexture.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = state.buildingTextureUpload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    ctx.commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    TransitionResource(ctx.commandList.Get(), state.buildingTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Now that buildingTexture exists, create its SRV in the pre-allocated descriptor
    ctx.device->CreateShaderResourceView(state.buildingTexture.Get(), &texSrvDesc, texSrvCpu);

     // Constant buffers
     const UINT sceneSize = Align256(sizeof(SceneConstants));
    CreateUploadBuffer(ctx.device.Get(), sceneSize, state.sceneCB);
    D3D12_RANGE emptyRange{ 0, 0 };
    state.sceneCB->Map(0, &emptyRange, reinterpret_cast<void**>(&state.sceneCbMapped));

    state.objectCbStride = Align256(sizeof(ObjectConstants));
    CreateUploadBuffer(ctx.device.Get(), static_cast<UINT64>(state.objectCbStride) * kMaxObjects, state.objectCB);
    state.objectCB->Map(0, &emptyRange, reinterpret_cast<void**>(&state.objectCbMapped));
}

inline void DrawCube(
    D3D12Context& ctx,
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& proj,
    const DirectX::XMFLOAT3& cameraPos,
    const D3D12_VIEWPORT& mainViewport,
    const RECT& mainScissor,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    using namespace SceneRendererInternal;
    auto& state = g_state;
    if (!state.litPipelineState) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float delta = std::chrono::duration<float>(now - state.lastAnimationTime).count();
    state.lastAnimationTime = now;
    state.cubeRotation += XMConvertToRadians(45.0f) * delta;

    UINT objIndex = 0;

    // Player
    XMMATRIX playerWorld = XMMatrixTranslation(g_gameState.playerPos.x, g_gameState.playerPos.y, g_gameState.playerPos.z);
    WriteObjectMatrix(state.objectCbMapped, state.objectCbStride, objIndex++, playerWorld);

    // Bullets
    UINT bulletStartIndex = objIndex;
    for (const auto& b : g_gameState.bullets) {
        if (objIndex >= kMaxObjects) break;
        XMMATRIX world = XMMatrixScaling(0.2f, 0.2f, 0.5f) * XMMatrixTranslation(b.position.x, b.position.y, b.position.z);
        WriteObjectMatrix(state.objectCbMapped, state.objectCbStride, objIndex++, world);
    }

    // Enemies
    UINT enemyStartIndex = objIndex;
    for (const auto& e : g_gameState.enemies) {
        if (objIndex >= kMaxObjects) break;
        XMMATRIX world = XMMatrixRotationY(state.cubeRotation) * XMMatrixTranslation(e.position.x, e.position.y, e.position.z);
        WriteObjectMatrix(state.objectCbMapped, state.objectCbStride, objIndex++, world);
    }
 
    // Buildings
    UINT buildingStartIndex = objIndex;
    for (const auto& b : g_gameState.buildings) {
        if (objIndex >= kMaxObjects) break;
        XMMATRIX world = XMMatrixScaling(b.scale.x, b.scale.y, b.scale.z) * XMMatrixTranslation(b.position.x, b.position.y, b.position.z);
        WriteObjectMatrix(state.objectCbMapped, state.objectCbStride, objIndex++, world);
    }

    // Ground (follows player Z)
    UINT groundIndex = objIndex;
    XMMATRIX groundWorld = XMMatrixTranslation(0.0f, kGroundHeight, g_gameState.playerPos.z);
    WriteObjectMatrix(state.objectCbMapped, state.objectCbStride, groundIndex, groundWorld);

    const XMVECTOR lightDirVec = XMVector3Normalize(XMVectorSet(0.3f, -1.0f, 0.2f, 0.0f));
    const XMVECTOR lightPos = XMVectorScale(lightDirVec, -20.0f);
    const XMVECTOR target = XMVectorZero();
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMMATRIX lightView = XMMatrixLookAtLH(lightPos, target, up);
    const XMMATRIX lightProj = XMMatrixOrthographicLH(30.0f, 30.0f, 0.1f, 100.0f);
    const XMMATRIX lightViewProj = lightView * lightProj;
    const XMMATRIX viewProj = view * proj;
    XMFLOAT3 lightDir{};
    XMStoreFloat3(&lightDir, lightDirVec);
    WriteSceneConstants(state, viewProj, lightViewProj, lightDir, cameraPos);

    // Shadow pass
    TransitionResource(ctx.commandList.Get(), state.shadowMap.Get(), state.shadowState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    state.shadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    ctx.commandList->SetPipelineState(state.shadowPipelineState.Get());
    ctx.commandList->SetGraphicsRootSignature(state.rootSignature.Get());
    ctx.commandList->SetGraphicsRootConstantBufferView(1, state.sceneCB->GetGPUVirtualAddress());
    ctx.commandList->RSSetViewports(1, &state.shadowViewport);
    ctx.commandList->RSSetScissorRects(1, &state.shadowScissor);
    ctx.commandList->OMSetRenderTargets(0, nullptr, FALSE, &state.shadowDsv);
    ctx.commandList->ClearDepthStencilView(state.shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    
    // Draw Player
    RecordDraw(ctx.commandList.Get(), state, state.ship, 0);
    // Draw Bullets
    for (size_t i = 0; i < g_gameState.bullets.size() && (bulletStartIndex + i) < kMaxObjects; ++i)
        RecordDraw(ctx.commandList.Get(), state, state.cube, bulletStartIndex + (UINT)i);
    // Draw Enemies
    for (size_t i = 0; i < g_gameState.enemies.size() && (enemyStartIndex + i) < kMaxObjects; ++i)
        RecordDraw(ctx.commandList.Get(), state, state.cube, enemyStartIndex + (UINT)i);
    // Draw Buildings
    for (size_t i = 0; i < g_gameState.buildings.size() && (buildingStartIndex + i) < kMaxObjects; ++i)
        RecordDraw(ctx.commandList.Get(), state, state.cube, buildingStartIndex + (UINT)i);
    // Draw Ground
    RecordDraw(ctx.commandList.Get(), state, state.ground, groundIndex);

    TransitionResource(ctx.commandList.Get(), state.shadowMap.Get(), state.shadowState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    state.shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // Main pass
    ctx.commandList->SetPipelineState(state.litPipelineState.Get());
    ctx.commandList->SetGraphicsRootSignature(state.rootSignature.Get());
    ctx.commandList->SetGraphicsRootConstantBufferView(1, state.sceneCB->GetGPUVirtualAddress());
    ctx.commandList->RSSetViewports(1, &mainViewport);
    ctx.commandList->RSSetScissorRects(1, &mainScissor);
    ctx.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
    ID3D12DescriptorHeap* heaps[] = { state.srvHeap.Get() };
    ctx.commandList->SetDescriptorHeaps(1, heaps);
    ctx.commandList->SetGraphicsRootDescriptorTable(2, state.shadowSrvGpu);
    ctx.commandList->SetGraphicsRootDescriptorTable(3, state.buildingSrvGpu);
    
    // Draw Player
    RecordDraw(ctx.commandList.Get(), state, state.ship, 0);
    // Draw Bullets
    for (size_t i = 0; i < g_gameState.bullets.size() && (bulletStartIndex + i) < kMaxObjects; ++i)
        RecordDraw(ctx.commandList.Get(), state, state.cube, bulletStartIndex + (UINT)i);
    // Draw Enemies
    for (size_t i = 0; i < g_gameState.enemies.size() && (enemyStartIndex + i) < kMaxObjects; ++i)
        RecordDraw(ctx.commandList.Get(), state, state.cube, enemyStartIndex + (UINT)i);
    // Draw Buildings
    for (size_t i = 0; i < g_gameState.buildings.size() && (buildingStartIndex + i) < kMaxObjects; ++i)
        RecordDraw(ctx.commandList.Get(), state, state.cube, buildingStartIndex + (UINT)i);
    // Draw Ground
    RecordDraw(ctx.commandList.Get(), state, state.ground, groundIndex);

    // Prepare for next frame
    TransitionResource(ctx.commandList.Get(), state.shadowMap.Get(), state.shadowState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    state.shadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
}
