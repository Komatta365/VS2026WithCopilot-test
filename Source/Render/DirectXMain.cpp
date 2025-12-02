#include "DirectXMain.h"
#include "DirectX12TriangleSample.h"
#include <d3dcompiler.h>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// DirectX 12 の初期化を行い、スワップチェーン/RTV/コマンドリスト等を構築する。
// パラメータ: hwnd=ターゲットウィンドウ、width/height=バックバッファサイズ
// 例外: 初期化に失敗した場合は std::runtime_error を送出
void InitD3D12(HWND hwnd, UINT width, UINT height)
{
    auto& ctx = GetD3D12Context();
    UINT dxgiFactoryFlags = 0;
#ifdef _DEBUG
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)))) {
        throw std::runtime_error("Failed to create DXGI factory");
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter1> hardwareAdapter;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &hardwareAdapter)); ++i) {
        DXGI_ADAPTER_DESC1 desc;
        hardwareAdapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&ctx.device)))) {
            break;
        }
    }
    if (!ctx.device) {
        throw std::runtime_error("Failed to create D3D12 device");
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ctx.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&ctx.commandQueue)))) {
        throw std::runtime_error("Failed to create command queue");
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(ctx.commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain))) {
        throw std::runtime_error("Failed to create swap chain");
    }
    if (FAILED(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER))) {
        throw std::runtime_error("Failed to make window association");
    }
    swapChain.As(&ctx.swapChain);
    ctx.frameIndex = ctx.swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(ctx.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&ctx.rtvHeap)))) {
        throw std::runtime_error("Failed to create RTV heap");
    }
    ctx.rtvDescriptorSize = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(ctx.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < FRAME_COUNT; ++i) {
        if (FAILED(ctx.swapChain->GetBuffer(i, IID_PPV_ARGS(&ctx.renderTargets[i])))) {
            throw std::runtime_error("Failed to get back buffer");
        }
        ctx.device->CreateRenderTargetView(ctx.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += ctx.rtvDescriptorSize;
    }
    if (FAILED(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx.commandAllocator)))) {
        throw std::runtime_error("Failed to create command allocator");
    }
    if (FAILED(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx.commandAllocator.Get(), nullptr, IID_PPV_ARGS(&ctx.commandList)))) {
        throw std::runtime_error("Failed to create command list");
    }
    ctx.commandList->Close();
    if (FAILED(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx.fence)))) {
        throw std::runtime_error("Failed to create fence");
    }
    ctx.fenceValue = 1;
    ctx.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!ctx.fenceEvent) {
        throw std::runtime_error("Failed to create fence event");
    }

    InitializeTrianglePipeline(ctx);
}

// D3D12 リソースの後始末（フェンスイベントのクローズ）。
void CleanupD3D12()
{
    auto& ctx = GetD3D12Context();
    if (ctx.fenceEvent) {
        CloseHandle(ctx.fenceEvent);
        ctx.fenceEvent = nullptr;
    }
}

// 毎フレームのゲームロジック更新（入力/物理/AI など）を行う。
// 現状はプレースホルダとして空実装。
void Update()
{
}

// 三角形描画フレームの発行処理。
// コマンド記録、リソース遷移、RTV クリア、描画、Present、フェンス待機までを行う。
void Render()
{
    auto& ctx = GetD3D12Context();
    // コマンドアロケータ/リストをリセットして記録準備
    ctx.commandAllocator->Reset();
    ctx.commandList->Reset(ctx.commandAllocator.Get(), ctx.pipelineState.Get());

    // バックバッファサイズを取得してビューポート/シザーを設定
    D3D12_RESOURCE_DESC backBufferDesc = ctx.renderTargets[ctx.frameIndex]->GetDesc();
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(backBufferDesc.Width), static_cast<float>(backBufferDesc.Height), 0.0f, 1.0f };
    ctx.commandList->RSSetViewports(1, &viewport);
    RECT scissor{ 0, 0, (LONG)backBufferDesc.Width, (LONG)backBufferDesc.Height };
    ctx.commandList->RSSetScissorRects(1, &scissor);

    // バックバッファを PRESENT -> RENDER_TARGET に遷移
    D3D12_RESOURCE_BARRIER barrier{};                                         // リソースの状態遷移を定義する構造体。
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;                    // バリアの種類を「状態遷移」に設定。
    barrier.Transition.pResource = ctx.renderTargets[ctx.frameIndex].Get();   // 遷移対象のリソース（現在のバックバッファ）を指定。
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;            // 現在の状態は「表示中 (PRESENT)」。
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;       // 次に使う状態は「レンダーターゲット (描画先)」。
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; // 全サブリソースを対象に遷移を行う。
    ctx.commandList->ResourceBarrier(1, &barrier);                            // コマンドリストにバリアを記録。

    // 現フレームの RTV を設定しクリアカラーで初期化
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(ctx.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvHandle.ptr += (SIZE_T)ctx.frameIndex * ctx.rtvDescriptorSize;
    ctx.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    const float clearColor[] = { 0.39f, 0.58f, 0.93f, 1.0f };
    ctx.commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // ルートシグネチャ/IA 設定して三角形を描画
    ctx.commandList->SetGraphicsRootSignature(ctx.rootSignature.Get());
    ctx.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx.commandList->IASetVertexBuffers(0, 1, &ctx.vertexBufferView);
    ctx.commandList->DrawInstanced(3, 1, 0, 0);

    // バックバッファを RENDER_TARGET -> PRESENT に遷移
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    ctx.commandList->ResourceBarrier(1, &barrier);

    // コマンドを閉じてキューへ投入、 Present 実行、フレーム完了待機
    ctx.commandList->Close();

    // GPUに対して「記録済みのコマンドリストを実行せよ」と指示する
    {
        ID3D12CommandList* lists[] = { ctx.commandList.Get() };
        ctx.commandQueue->ExecuteCommandLists(_countof(lists), lists);
    }

    // 「描画結果を画面に出す」ための最終ステップで、垂直同期を有効にしてスワップする
    //
    // バックバッファをフロントバッファに切り替えて、画面に描画結果を表示する
    // 第1引数：SyncInterval
    //    0 → 垂直同期なし（即時表示、ティアリングが発生する可能性あり）
    //    1 → 垂直同期あり（1フレーム分待つ、通常はこれ）
    //    2 → 2フレーム分待つ（低FPS向け）
    // 第2引数：Flags
    //    通常は 0（特別なオプションなし） DXGI_PRESENT_DO_NOT_WAIT などを指定すると、非同期表示を試みる
    {
        ctx.swapChain->Present(1, 0);
    }

    // 直前フレームの完了を待機してからフレームインデックスを更新
    WaitForPreviousFrame();
}

// 直前フレームの GPU 完了をフェンスで待機し、バックバッファのフレームインデックスを更新する。
void WaitForPreviousFrame()
{
    auto& ctx = GetD3D12Context();

    // 現在発行したコマンド列の完了値（フェンス値）を記録する
    // Signal は GPU 側でコマンドが到達したタイミングでフェンス値を設定するため、
    // この値をもとに CPU が「どこまで終わったか」を判定できる
    const UINT64 currentFenceValue = ctx.fenceValue;
    ctx.commandQueue->Signal(ctx.fence.Get(), currentFenceValue);

    // もし GPU の完了値が currentFenceValue に達していない場合は、イベントを使って待機
    // SetEventOnCompletion は GPU 完了時に OS イベントをシグナルし、WaitForSingleObject でブロックする
    if (ctx.fence->GetCompletedValue() < currentFenceValue) {
        // GPUの処理が指定したフェンス値に達したときに、指定したイベントをシグナル状態にするための設定です。
        // つまり、GPUの完了を待つためのイベント通知を登録する処理です。
        ctx.fence->SetEventOnCompletion(currentFenceValue, ctx.fenceEvent);

        // 指定したイベントがシグナル状態になるまで、無限に待機します。
        WaitForSingleObject(ctx.fenceEvent, INFINITE);
    }

    // 次フレーム用にフェンス値をインクリメント（以降のフレームはこの新しい値をターゲットに待機する）
    ++ctx.fenceValue;

    // スワップチェーンの現在のバックバッファインデックスを取得
    // これで次フレームが使用すべきバックバッファを特定できる（2重/3重バッファリング対応）
    ctx.frameIndex = ctx.swapChain->GetCurrentBackBufferIndex();
}
