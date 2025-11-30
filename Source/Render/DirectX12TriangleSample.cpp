#include "DirectX12TriangleSample.h"
#include "DirectXMain.h" // D3D12Context の完全定義が必要
#include <d3dcompiler.h>
#include <stdexcept>
#include <cstring>

// 最小構成の DirectX 12 パイプラインを初期化して三角形を描画できるようにする。
// 引数:
//  - ctx: 共有コンテキスト（ルートシグネチャ、PSO、頂点バッファを格納）
// 例外:
//  - シェーダコンパイルや D3D12 オブジェクト生成に失敗した場合は std::runtime_error を送出
void InitializeTrianglePipeline(D3D12Context& ctx)
{
    // 入力アセンブラのみを許可する空のルートシグネチャを作成
    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> serializedRS;
    Microsoft::WRL::ComPtr<ID3DBlob> errorRS;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRS, &errorRS))) {
        throw std::runtime_error("ルートシグネチャのシリアライズに失敗");
    }
    if (FAILED(ctx.device->CreateRootSignature(0, serializedRS->GetBufferPointer(), serializedRS->GetBufferSize(), IID_PPV_ARGS(&ctx.rootSignature)))) {
        throw std::runtime_error("ルートシグネチャの作成に失敗");
    }

    // インライン HLSL から単純な頂点/ピクセルシェーダをコンパイル
    const char* vsSrc = R"(
        struct VSInput { float2 pos : POSITION; float3 col : COLOR; };
        struct PSInput { float4 pos : SV_Position; float3 col : COLOR; };
        PSInput main(VSInput input) {
            PSInput o; o.pos = float4(input.pos, 0.0f, 1.0f); o.col = input.col; return o;
        }
    )";
    const char* psSrc = R"(
        struct PSInput { float4 pos : SV_Position; float3 col : COLOR; };
        float4 main(PSInput input) : SV_Target {
            return float4(input.col, 1.0f);
        }
    )";
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, compileErr;
    if (FAILED(D3DCompile(vsSrc, std::strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &compileErr))) {
        throw std::runtime_error("頂点シェーダのコンパイルに失敗");
    }
    if (FAILED(D3DCompile(psSrc, std::strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &compileErr))) {
        throw std::runtime_error("ピクセルシェーダのコンパイルに失敗");
    }

    // 頂点入力レイアウト（位置と色）を定義
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // 不透明な三角形向けのブレンド/ラスタライズ/深度ステンシルを設定
    D3D12_BLEND_DESC blendDesc{};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.BlendEnable = FALSE;
    rtBlend.LogicOpEnable = FALSE;
    rtBlend.SrcBlend = D3D12_BLEND_ONE;
    rtBlend.DestBlend = D3D12_BLEND_ZERO;
    rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blendDesc.RenderTarget[0] = rtBlend;

    D3D12_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.AntialiasedLineEnable = FALSE;
    rasterDesc.ForcedSampleCount = 0;
    rasterDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;    // 2D 三角形のため深度は無効
    depthDesc.StencilEnable = FALSE;

    // 三角形用のパイプラインステートオブジェクト（PSO）を作成
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = ctx.rootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState = blendDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = rasterDesc;
    psoDesc.DepthStencilState = depthDesc;
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    if (FAILED(ctx.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&ctx.pipelineState)))) {
        throw std::runtime_error("PSO の作成に失敗");
    }

    // 3 つの色付き頂点を持つ頂点バッファを作成してアップロード
    struct Vertex { float x, y; float r, g, b; };
    Vertex triangle[] = {
        {  0.0f,  0.5f, 1.0f, 0.0f, 0.0f },
        {  0.5f, -0.5f, 0.0f, 1.0f, 0.0f },
        { -0.5f, -0.5f, 0.0f, 0.0f, 1.0f },
    };
    const UINT vbSize = sizeof(triangle);

    D3D12_HEAP_PROPERTIES heapProps{}; heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width = vbSize;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc = { 1, 0 };
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(ctx.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ctx.vertexBuffer))))
        throw std::runtime_error("頂点バッファの作成に失敗");

    // バッファをマップして頂点データをコピー
    void* mapped = nullptr; D3D12_RANGE readRange{0,0};
    ctx.vertexBuffer->Map(0, &readRange, &mapped);
    std::memcpy(mapped, triangle, vbSize);
    ctx.vertexBuffer->Unmap(0, nullptr);

    // 入力アセンブラで使用する頂点バッファビューを設定
    ctx.vertexBufferView.BufferLocation = ctx.vertexBuffer->GetGPUVirtualAddress();
    ctx.vertexBufferView.StrideInBytes = sizeof(Vertex);
    ctx.vertexBufferView.SizeInBytes = vbSize;
}
