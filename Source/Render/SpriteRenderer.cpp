#include "SpriteRenderer.h"
#include <d3dcompiler.h>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <wincodec.h>
#include <objbase.h>

static ComPtr<ID3D12DescriptorHeap> g_srvHeap;
static D3D12_GPU_DESCRIPTOR_HANDLE g_srvGpu{};
static ComPtr<ID3D12Resource> g_texture;
static ComPtr<ID3D12Resource> g_upload;
static ComPtr<ID3D12Resource> g_cb;
static UINT g_srvDescriptorSize = 0;
static ComPtr<ID3D12RootSignature> g_rootSignature;
static ComPtr<ID3D12PipelineState> g_pipelineState;

// HRESULT を検査して失敗時に例外を投げるユーティリティ。
// 引数: hr=API の戻り値, msg=例外メッセージ
static void ThrowIfFailed(HRESULT hr, const char* msg)
{
    // 失敗コードの場合のみ例外化。成功時は何もしない。
    if (FAILED(hr))
        throw std::runtime_error(msg);
}

// WIC を用いて PNG/JPEG 等の画像ファイルを読み込み、
// D3D12 のデフォルトヒープ上に 2D テクスチャを作成し SRV を用意する。
// 引数: ctx=D3D12 コンテキスト, path=読み込む画像ファイルのパス（UTF-16）
// 例外: 失敗時は std::runtime_error を送出
/// <summary>
/// Creates a Direct3D 12 texture resource from an image file using Windows Imaging Component (WIC).
/// 
/// This function performs the following operations:
/// 1. Initializes COM in multithreaded mode
/// 2. Creates a WIC imaging factory (with fallback support for older versions)
/// 3. Decodes the image file and retrieves the first frame
/// 4. Converts the image to RGBA8 format for GPU compatibility
/// 5. Copies pixel data to CPU memory
/// 6. Creates a GPU texture resource in the default heap
/// 7. Creates an upload buffer for data transfer
/// 8. Synchronizes GPU execution
/// 9. Maps and copies pixel data to the upload buffer
/// 10. Executes a command list to copy data from upload buffer to GPU texture
/// 11. Transitions the texture resource to pixel shader resource state
/// 12. Waits for GPU completion
/// 13. Creates a shader resource view (SRV) for shader access
/// 
/// Note: This function uses global variables g_texture, g_upload, and g_srvHeap.
/// It is recommended to refactor these as class members or function parameters for better encapsulation.
/// </summary>
/// <param name="ctx">Reference to the D3D12Context containing device, command queue, command allocator, command list, fence, and fence event</param>
/// <param name="path">Wide character string path to the image file to be loaded</param>
/// <exception cref="std::runtime_error">Thrown if COM initialization fails with unexpected HRESULT</exception>
/// <exception cref="DirectX::com_exception">Thrown by ThrowIfFailed if any WIC or Direct3D 12 operation fails</exception>
static void CreateTextureFromFileWIC(D3D12Context& ctx, const wchar_t* path)
{
    // COM 初期化（既に別モードで初期化済みでも許容）。
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (comHr != S_OK && comHr != S_FALSE && comHr != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("COM initialization failed");
    }

    // WIC ファクトリの作成。新しい CLSID で試し、失敗時は旧 CLSID にフォールバック。
    ComPtr<IWICImagingFactory> factory;
    HRESULT facHr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(facHr)) {
        ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC factory failed");
    }

    // デコーダ生成と最初のフレーム取得。
    ComPtr<IWICBitmapDecoder> decoder;
    ThrowIfFailed(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "WIC decoder failed");
    ComPtr<IWICBitmapFrameDecode> frame;
    ThrowIfFailed(decoder->GetFrame(0, &frame), "WIC frame failed");

    // 画像サイズの取得。
    UINT w = 0, h = 0;
    ThrowIfFailed(frame->GetSize(&w, &h), "WIC size failed");

    // ピクセルフォーマットを RGBA8 に変換（GPU で扱いやすい形式）。
    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(factory->CreateFormatConverter(&converter), "WIC converter failed");
    ThrowIfFailed(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "WIC convert failed");

    // 変換結果を CPU メモリへ展開。
    std::vector<BYTE> pixels(w * h * 4);
    ThrowIfFailed(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()), "WIC copy failed");

    // GPU デフォルトヒープにテクスチャリソースを作成（初期状態はコピー先）。
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc = { 1, 0 };
    D3D12_HEAP_PROPERTIES defHP{};
    defHP.Type = D3D12_HEAP_TYPE_DEFAULT;
    ThrowIfFailed(ctx.device->CreateCommittedResource(&defHP, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_texture)), "Create texture failed");

    // アップロード用の中間バッファを作成。
    UINT64 uploadSize{};
    ctx.device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
    D3D12_RESOURCE_DESC upDesc{};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width = uploadSize;
    upDesc.Height = 1;
    upDesc.DepthOrArraySize = 1;
    upDesc.MipLevels = 1;
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    upDesc.SampleDesc = { 1, 0 };
    D3D12_HEAP_PROPERTIES upHP{};
    upHP.Type = D3D12_HEAP_TYPE_UPLOAD;
    ThrowIfFailed(ctx.device->CreateCommittedResource(&upHP, D3D12_HEAP_FLAG_NONE, &upDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_upload)), "Create upload failed");

    {
        ctx.commandQueue->Signal(ctx.fence.Get(), ++ctx.fenceValue);
        void* p = nullptr;
        ctx.fence->SetEventOnCompletion(ctx.fenceValue, ctx.fenceEvent);
        WaitForSingleObject(ctx.fenceEvent, INFINITE);
        ctx.commandAllocator->Reset();
        ctx.commandList->Reset(ctx.commandAllocator.Get(), nullptr);
    }

    // アップロードバッファにピクセルを書き込み。
    void* upMapped{};
    D3D12_RANGE rr{ 0, 0 };
    g_upload->Map(0, &rr, &upMapped);
    memcpy(upMapped, pixels.data(), pixels.size());
    g_upload->Unmap(0, nullptr);

    // 一時コマンドでコピー＋状態遷移。
    ComPtr<ID3D12CommandAllocator> alloc;
    ThrowIfFailed(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "Temp alloc failed");
    ComPtr<ID3D12GraphicsCommandList> list;
    ThrowIfFailed(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "Temp CL failed");
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    ctx.device->GetCopyableFootprints(&texDesc, 0, 1, 0, &src.PlacedFootprint, nullptr, nullptr, nullptr);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    // コピー先からピクセルシェーダ用の読み取り状態へ遷移。
    D3D12_RESOURCE_BARRIER toSRV{};
    toSRV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSRV.Transition.pResource = g_texture.Get();
    toSRV.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSRV.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &toSRV);
    list->Close();
    ID3D12CommandList* lists2[] = { list.Get() };
    ctx.commandQueue->ExecuteCommandLists(1, lists2);

    // フェンスで GPU 完了待ち（テクスチャ準備が終わるまでブロック）。
    ComPtr<ID3D12Fence> tempFence;
    ThrowIfFailed(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tempFence)), "Temp fence failed");
    HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ctx.commandQueue->Signal(tempFence.Get(), 1);
    tempFence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);

    // SRV を作成してシェーダから参照できるようにする。
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ctx.device->CreateShaderResourceView(g_texture.Get(), &srv, g_srvHeap->GetCPUDescriptorHandleForHeapStart());
}

// スプライト描画に必要な D3D12 パイプライン（RS/PSO/VB/SRV/CB）を初期化する。
// 引数: ctx=D3D12 コンテキスト（デバイス/キュー等を保持）
// 例外: 失敗時は std::runtime_error を送出
void InitializeSpritePipeline(D3D12Context& ctx)
{
    // SRV(t0) と CBV(b0)、静的サンプラ(s0) を持つルートシグネチャを構築。
    D3D12_DESCRIPTOR_RANGE ranges[1]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0; // t0
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    // VS/PS 両方から参照される CBV(b0)。行列などを渡す。
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0; // b0
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // PS から参照される SRV(t0) をテーブルで渡す（将来拡張しやすい）。
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = ranges;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // サンプラ s0 を静的にバインド（ヒープ不要、固定挙動）。
    D3D12_STATIC_SAMPLER_DESC staticSampler{};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.MipLODBias = 0.0f;
    staticSampler.MaxAnisotropy = 1;
    staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    staticSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    staticSampler.MinLOD = 0.0f;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister = 0; // s0
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = _countof(rootParams);
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &staticSampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // RS のシリアライズと作成。
    ComPtr<ID3DBlob> rsBlob, rsErr;
    ThrowIfFailed(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr), "Serialize RS failed");
    ThrowIfFailed(ctx.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature)), "Create RS failed");

    // シェーダのコンパイル（頂点/ピクセル）。
    ComPtr<ID3DBlob> vs, ps, err;
    ThrowIfFailed(D3DCompileFromFile(L"Asset/Shader/SpriteVS.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &err), "Compile VS failed");
    ThrowIfFailed(D3DCompileFromFile(L"Asset/Shader/SpritePS.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &err), "Compile PS failed");

    // 入力レイアウト（位置3/UV2）。
    D3D12_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // PSO 設定（不透明描画、カリング無し、深度無効）。
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState = rast;
    pso.DepthStencilState = depth;
    pso.InputLayout = { il, _countof(il) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    ThrowIfFailed(ctx.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineState)), "Create PSO failed");

    // 画面アスペクトに合わせたフルスクリーン相当の四角形を用意。
    struct V {
        float px, py, pz;
        float u, v;
    };
    const float aspectH = 720.0f / 1280.0f;
    V quad[] = {
        { -aspectH, -1, 0, 0, 1 },
        { -aspectH, 1, 0, 0, 0 },
        { aspectH, -1, 0, 1, 1 },
        { aspectH, 1, 0, 1, 0 },
    };

    // アップロードヒープに頂点バッファを作成し、即時書き込み。
    UINT vbSize = sizeof(quad);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rb{};
    rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb.Width = vbSize;
    rb.Height = 1;
    rb.DepthOrArraySize = 1;
    rb.MipLevels = 1;
    rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rb.SampleDesc = { 1, 0 };
    rb.Format = DXGI_FORMAT_UNKNOWN;
    ThrowIfFailed(ctx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ctx.vertexBuffer)), "Create VB failed");
    void* mapped{};
    D3D12_RANGE rr{ 0, 0 };
    ctx.vertexBuffer->Map(0, &rr, &mapped);
    memcpy(mapped, quad, vbSize);
    ctx.vertexBuffer->Unmap(0, nullptr);
    ctx.vertexBufferView.BufferLocation = ctx.vertexBuffer->GetGPUVirtualAddress();
    ctx.vertexBufferView.StrideInBytes = sizeof(V);
    ctx.vertexBufferView.SizeInBytes = vbSize;

    // SRV ヒープを作成し、GPU から参照できるように設定。
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.NumDescriptors = 1;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(ctx.device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap)), "Create SRV heap failed");
    g_srvDescriptorSize = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_srvGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();

    // 画像ファイルからテクスチャを作成し、SRV(t0) を用意。
    CreateTextureFromFileWIC(ctx, L"Asset/Texture/CatSprite2.png");

    // 定数バッファ（単位行列）を作成して b0 にバインドするためのリソースを用意。
    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = 256;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDesc.SampleDesc = { 1, 0 };
    D3D12_HEAP_PROPERTIES cbHP{};
    cbHP.Type = D3D12_HEAP_TYPE_UPLOAD;
    ThrowIfFailed(ctx.device->CreateCommittedResource(&cbHP, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_cb)), "Create CB failed");
    float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    void* cbMap{};
    g_cb->Map(0, &rr, &cbMap);
    std::memcpy(cbMap, identity, sizeof(identity));
    g_cb->Unmap(0, nullptr);
}

// スプライト四角形を描画するためのコマンドを記録する。
// 引数: ctx=D3D12 コンテキスト（コマンドリスト等を保持）
void DrawSprite(D3D12Context& ctx)
{
    // スプライト用の RS/PSO をセット。
    ctx.commandList->SetGraphicsRootSignature(g_rootSignature.Get());
    ctx.commandList->SetPipelineState(g_pipelineState.Get());

    // b0 に定数バッファ（変換行列）をバインド。
    ctx.commandList->SetGraphicsRootConstantBufferView(0, g_cb->GetGPUVirtualAddress());

    // SRV ヒープをコマンドリストへバインドし、t0 の場所を指定。
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    ctx.commandList->SetDescriptorHeaps(1, heaps);
    ctx.commandList->SetGraphicsRootDescriptorTable(1, g_srvGpu);

    // IA 設定（プリミティブは TRIANGLESTRIP、頂点バッファをセット）。
    ctx.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx.commandList->IASetVertexBuffers(0, 1, &ctx.vertexBufferView);

    // 4頂点のストリップで矩形を描画。
    ctx.commandList->DrawInstanced(4, 1, 0, 0);
}
