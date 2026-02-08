// DirectX 12 メインレンダリング エンジン
// このモジュールは、DirectX 12 デバイスの初期化、コマンド実行、フレームレンダリングを管理します。
// 特徴：
//   - Data-driven camera system: カメラ参照ポイントでの計算負荷最小化
//   - Pool-based rendering: 複数レンダラーの統一管理（三角形/スプライト/立方体）
//   - GPU フェンスシンク: ダブルバッファリングによるフレーム同期

#include "DirectXMain.h"
#include "DirectX12TriangleSample.h"
#include "SpriteRenderer.h"
#include "CubeRenderer.h"
#include "../Game/GameLoop.h"

#include <DirectXMath.h>
#include <chrono>
#include <algorithm>
#include <d3dcompiler.h>

using namespace DirectX;

namespace {
// ========================================
// カメラ状態構造体
// ========================================
// RPGゲーム向けの球面座標系カメラ
// 意図: プレイヤー中心の常時追従カメラで、pitch/yaw による自由な視点制御を実現
struct CameraState {
    XMFLOAT3 position{ 0.0f, 0.0f, -6.0f }; // ワールド座標でのカメラ位置
    float yaw = 0.0f;                       // Y軸周りの回転角（ラジアン）
    float pitch = 0.3f;                     // X軸周りの回転角（ラジアン）
    float radius = 10.0f;                   // プレイヤーからの距離
    XMMATRIX view{ XMMatrixIdentity() };    // View行列（毎フレーム更新）
    XMMATRIX proj{ XMMatrixIdentity() };    // Projection行列（初期化後は変更なし）
};

CameraState g_camera;                                                                     // グローバルカメラ状態
std::chrono::steady_clock::time_point g_prevFrameTime = std::chrono::steady_clock::now(); // 前フレーム時刻（deltaTime計算用）

void UpdateCamera(float deltaSeconds);

// ========================================
// InitializeCamera
// ========================================
// 用途: ゲーム開始時のカメラシステム初期化
// 処理:
//   1. カメラパラメータの初期化（球面座標系、距離10ユニット）
//   2. アスペクト比に基づいた射影行列の設定（FOV 60度）
//   3. フレーム時刻の同期（deltaTime計算の基準点）
void InitializeCamera(float width, float height)
{
    g_camera.radius = 10.0f;
    g_camera.yaw = 0.0f;
    g_camera.pitch = 0.3f;
    // 視野角 60度、アスペクト比 width/height、ニアクリップ 0.1f、ファークリップ 100.0f
    g_camera.proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), width / height, 0.1f, 100.0f);
    g_prevFrameTime = std::chrono::steady_clock::now();
    UpdateCamera(0.0f);
}

// ========================================
// UpdateCamera
// ========================================
// 用途: 毎フレームのカメラパラメータ更新（キー入力に応答）
// パラメータ: deltaSeconds - 前フレームからの経過時間（秒、最大 0.1秒にクランプ）
// 処理:
//   1. 入力キー（矢印キー）の検出と yaw/pitch の更新
//   2. pitch の制限（上下180度を回避）
//   3. 球面座標系 → デカルト座標系への変換
//   4. View行列の再計算（原点を向く視点）
void UpdateCamera(float deltaSeconds)
{
    deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.1f);
    const float rotateSpeed = XMConvertToRadians(60.0f);

    // 矢印キーの入力検出と回転角の更新（毎フレーム60度回転可能）
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) {
        g_camera.yaw -= rotateSpeed * deltaSeconds;
    }
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) {
        g_camera.yaw += rotateSpeed * deltaSeconds;
    }
    if (GetAsyncKeyState(VK_UP) & 0x8000) {
        g_camera.pitch += rotateSpeed * deltaSeconds;
    }
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
        g_camera.pitch -= rotateSpeed * deltaSeconds;
    }

    // ジンバルロック回避: pitch を [-π/2 + 0.1, π/2 - 0.1] に制限
    g_camera.pitch = std::clamp(g_camera.pitch, -XM_PIDIV2 + 0.1f, XM_PIDIV2 - 0.1f);

    // 球面座標系 → デカルト座標系への変換
    // (radius, pitch, yaw) -> (x, y, z)
    const float x = g_camera.radius * cosf(g_camera.pitch) * sinf(g_camera.yaw);
    const float y = g_camera.radius * sinf(g_camera.pitch);
    const float z = g_camera.radius * cosf(g_camera.pitch) * cosf(g_camera.yaw);
    g_camera.position = { x, y, z };

    // View行列の再計算: カメラ位置から原点(0,0,0)を見上向きベクトル(0,1,0)で
    XMVECTOR eye = XMLoadFloat3(&g_camera.position);
    g_camera.view = XMMatrixLookAtLH(eye, XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
}

// ========================================
// ComputeDeltaSeconds
// ========================================
// 用途: 前フレームからの経過時間を計算（固定タイムステップの基礎）
// 戻り値: フレーム間隔（秒単位の浮動小数点値）
float ComputeDeltaSeconds()
{
    const auto now = std::chrono::steady_clock::now();
    const float delta = std::chrono::duration<float>(now - g_prevFrameTime).count();
    g_prevFrameTime = now;
    return delta;
}
} // namespace

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ========================================
// InitD3D12
// ========================================
// 用途: DirectX 12 デバイス・スワップチェーン・レンダリングパイプラインの初期化
// パラメータ:
//   - hwnd: ゲームウィンドウハンドル
//   - width, height: バックバッファサイズ（ピクセル）
// 戻り値: true = 初期化成功、false = 初期化失敗（詳細はログで確認推奨）
// 処理ステップ:
//   1. Debug層の有効化（_DEBUGビルド時）
//   2. DXGIファクトリの生成
//   3. GPU適応の列挙と D3D12 デバイスの生成
//   4. コマンドキュー・スワップチェーンの初期化
//   5. RTV（レンダーターゲットビュー）と DSV（深度ステンシルビュー）ヒープの作成
//   6. バックバッファとの関連付け
//   7. コマンド記録オブジェクトの生成（アロケータ・リスト）
//   8. GPU同期用フェンスの初期化
//   9. 各レンダPipelineの初期化（三角形・スプライト・立方体）
// 10. カメラの初期化

// DirectX 12 の初期化を行い、スワップチェーン/RTV/コマンドリスト等を構築する。
// パラメータ: hwnd=ターゲットウィンドウ、width/height=バックバッファサイズ
// 戻り値: 成功時 true、失敗時 false
bool InitD3D12(HWND hwnd, UINT width, UINT height)
{
    auto& ctx = GetD3D12Context();
    UINT dxgiFactoryFlags = 0;
#ifdef _DEBUG
    // Debug ビルド時: GPU デバッグレイヤーを有効化（性能低下あり）
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    // DXGI ファクトリの生成（GPU列挙・スワップチェーン生成用）
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)))) {
        return false;
    }

    // ハードウェアアダプタの列挙と D3D12 デバイスの生成
    // ソフトウェアアダプタをスキップし、最初にデバイス生成に成功したアダプタを選択
    Microsoft::WRL::ComPtr<IDXGIAdapter1> hardwareAdapter;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &hardwareAdapter)); ++i) {
        DXGI_ADAPTER_DESC1 desc;
        hardwareAdapter->GetDesc1(&desc);
        // ソフトウェアレンダラーをスキップ（WARP）
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        // Feature Level 11.0 以上をサポートするデバイスを生成
        if (SUCCEEDED(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&ctx.device)))) {
            break;
        }
    }
    if (!ctx.device) {
        return false;
    }

    // コマンドキューの生成（GPU に対する命令送信用）
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ctx.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&ctx.commandQueue)))) {
        return false;
    }

    // スワップチェーンの設定
    // FLIP_DISCARD: フリップモデル（最新のパフォーマンス最適化）、不要なバッファをスキップ
    // BufferCount: 2=ダブルバッファ（60FPS時は16ms以内に完了必須）
    //            3=トリプルバッファ（フレーム処理が16msを超える場合に推奨）
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.BufferCount = FRAME_COUNT; // ダブルバッファリング
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // 32-bit RGBA
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // モダンフリップモデル
    swapChainDesc.SampleDesc.Count = 1;                       // マルチサンプリング無効（単一サンプル）

    // スワップチェーンの生成（CreateSwapChainForHwnd）
    // 重要: pDevice パラメータ（ctx.commandQueue）は ID3D12Device ではなく
    //      ID3D12CommandQueue へのポインタである必要があります
    // IDXGIFactory2 以上が必要（CreateSwapChainForHwnd は DXGI 1.2+ API）
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(ctx.commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain))) {
        return false;
    }
    // Alt+Enter フルスクリーン切り替えを無効化
    if (FAILED(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER))) {
        return false;
    }
    swapChain.As(&ctx.swapChain);
    ctx.currentBackBufferIndex = ctx.swapChain->GetCurrentBackBufferIndex();

    // RTV（レンダーターゲットビュー）ヒープの生成
    // 各バックバッファに対応する RTV ディスクリプタを配置
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(ctx.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&ctx.rtvHeap)))) {
        return false;
    }
    // RTV ディスクリプタのサイズを記録（ポインタ操作用）
    ctx.rtvDescriptorSize = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // スワップチェーンから各バックバッファを取得し、対応する RTV を作成
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(ctx.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < FRAME_COUNT; ++i) {
        if (FAILED(ctx.swapChain->GetBuffer(i, IID_PPV_ARGS(&ctx.renderTargets[i])))) {
            return false;
        }
        ctx.device->CreateRenderTargetView(ctx.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += ctx.rtvDescriptorSize;
    }

    // DSV（深度ステンシルビュー）ヒープの生成
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(ctx.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&ctx.dsvHeap)))) {
        return false;
    }

    // 深度バッファの設定（32-bit float フォーマット）
    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc = { 1, 0 };
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES depthHeapProps{};
    depthHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // 深度バッファのクリア値（最大深度 1.0）
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;
    depthClear.DepthStencil.Stencil = 0;

    // 深度リソースの生成
    if (FAILED(ctx.device->CreateCommittedResource(
            &depthHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthClear,
            IID_PPV_ARGS(&ctx.depthStencil)))) {
        return false;
    }

    // 深度ステンシルビューの作成
    ctx.device->CreateDepthStencilView(ctx.depthStencil.Get(), nullptr, ctx.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // コマンドアロケータの生成（メモリアロケーショマネージャ）
    if (FAILED(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx.commandAllocator)))) {
        return false;
    }

    // コマンドリストの生成（GPU コマンド記録用）
    if (FAILED(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx.commandAllocator.Get(), nullptr, IID_PPV_ARGS(&ctx.commandList)))) {
        return false;
    }
    ctx.commandList->Close();

    // GPU 同期用フェンスの生成
    if (FAILED(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx.fence)))) {
        return false;
    }
    ctx.fenceValue = 1;
    // フェンス完了イベント（WaitForSingleObject で待機用）
    ctx.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!ctx.fenceEvent) {
        return false;
    }

    // 各レンダリングパイプラインの初期化
    // 三角形、スプライト、立方体の3つのパイプラインを管理
    try {
        InitializeTrianglePipeline(ctx);
        InitializeSpritePipeline(ctx);
        InitializeCubeRenderer(ctx);
    }
    catch (...) {
        return false;
    }

    // カメラシステムの初期化
    InitializeCamera(static_cast<float>(width), static_cast<float>(height));

    return true;
}

// ========================================
// CleanupD3D12
// ========================================
// 用途: DirectX 12 リソースのクリーンアップ
// 処理: フェンスイベント（Windows オブジェクト）のクローズ
//       （COM インターフェイスの自動解放は ComPtr デストラクタに委譲）

// D3D12 リソースの後始末（フェンスイベントのクローズ）。
void CleanupD3D12()
{
    auto& ctx = GetD3D12Context();
    if (ctx.fenceEvent) {
        CloseHandle(ctx.fenceEvent);
        ctx.fenceEvent = nullptr;
    }
}

// ========================================
// Update
// ========================================
// 用途: 毎フレーム呼び出されるゲームロジック更新関数
// 処理:
//   1. フレーム時間差分の計算
//   2. ゲーム状態の更新（物理・AI・入力など）
//   3. カメラをプレイヤーに追従させる

// 毎フレームのゲームロジック更新（入力/物理/AI など）を行う。
void Update()
{
    const float deltaSeconds = ComputeDeltaSeconds();
    UpdateGame(deltaSeconds);

    // カメラがプレイヤーを常に追従（3人称視点）
    // プレイヤーの 5ユニット上・10ユニット後ろから見下ろす視点
    g_camera.position = { g_gameState.playerPos.x, g_gameState.playerPos.y + 5.0f, g_gameState.playerPos.z - 10.0f };
    g_camera.view = XMMatrixLookAtLH(XMLoadFloat3(&g_camera.position),
                                     XMLoadFloat3(&g_gameState.playerPos),
                                     XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
}

// ========================================
// Render
// ========================================
// 用途: 毎フレームのレンダリング処理（GPU コマンド発行）
// 処理:
//   1. コマンドアロケータ/コマンドリストのリセット
//   2. ビューポート・シザーテストの設定
//   3. バックバッファのリソース遷移（PRESENT -> RENDER_TARGET）
//   4. RTV・DSV のセット、クリア
//   5. 各レンダラーの描画コマンド実行
//      - 三角形（基本テスト用）
//      - スプライト（UI/2D）
//      - 立方体（3D）
//   6. バックバッファのリソース遷移（RENDER_TARGET -> PRESENT）
//   7. コマンドリスト送信・GPU 実行
//   8. Present（スワップチェーン表示）
//   9. フェンス同期（前フレームの GPU 完了を待機）

// 三角形描画フレームの発行処理。
// コマンド記録、リソース遷移、RTV クリア、描画、Present、フェンス待機までを行う。
void Render()
{
    auto& ctx = GetD3D12Context();
    // コマンドアロケータ/リストをリセットして記録準備
    {
        ctx.commandAllocator->Reset();
        ctx.commandList->Reset(ctx.commandAllocator.Get(), ctx.pipelineState.Get());
    }

    // ビューポート・シザーテストの設定（バックバッファサイズに合わせる）
    D3D12_RESOURCE_DESC backBufferDesc = ctx.renderTargets[ctx.currentBackBufferIndex]->GetDesc();
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(backBufferDesc.Width), static_cast<float>(backBufferDesc.Height), 0.0f, 1.0f };
    ctx.commandList->RSSetViewports(1, &viewport);
    RECT scissor{ 0, 0, static_cast<LONG>(backBufferDesc.Width), static_cast<LONG>(backBufferDesc.Height) };
    ctx.commandList->RSSetScissorRects(1, &scissor);

    // バックバッファを PRESENT -> RENDER_TARGET に遷移
    // 重要: Present() 呼び出し前に、バックバッファは必ず D3D12_RESOURCE_STATE_PRESENT 状態である必要があります
    //      そうでない場合、Present は DXGI_ERROR_INVALID_CALL で失敗します
    // （レンダリング対象として使用可能にする）
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = ctx.renderTargets[ctx.currentBackBufferIndex].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ctx.commandList->ResourceBarrier(1, &barrier);
    }

    // RTV（レンダーターゲットビュー）および DSV（深度ステンシルビュー）のハンドル取得
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(ctx.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvHandle.ptr += static_cast<SIZE_T>(ctx.currentBackBufferIndex) * ctx.rtvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle(ctx.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // RTV/DSV を設定してクリア
    {
        ctx.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        constexpr float clearColor[] = { 0.15f, 0.15f, 0.15f, 1.0f };
        ctx.commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        ctx.commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    // 三角形描画（テスト用の基本ジオメトリ）
    {
        ctx.commandList->SetGraphicsRootSignature(ctx.rootSignature.Get());
        ctx.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx.commandList->IASetVertexBuffers(0, 1, &ctx.vertexBufferView);
        ctx.commandList->DrawInstanced(3, 1, 0, 0);
    }

    // スプライト描画（2D UI など）
    {
        DrawSprite(ctx);
    }

    // 3D シーン描画（立方体やその他の 3D オブジェクト）
    {
        DrawCube(ctx, g_camera.view, g_camera.proj, g_camera.position, viewport, scissor, rtvHandle, dsvHandle);
    }

    // バックバッファを RENDER_TARGET -> PRESENT に遷移
    // 注釈: Present() 呼び出し直前に、バックバッファをこの状態にする必要があります
    //      （スワップチェーンの仕様により強制）
    // （画面に表示可能な状態にする）
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = ctx.renderTargets[ctx.currentBackBufferIndex].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ctx.commandList->ResourceBarrier(1, &barrier);
    }

    ctx.commandList->Close();

    // コマンドリストを GPU に送信・実行
    {
        ID3D12CommandList* lists[] = { ctx.commandList.Get() };
        ctx.commandQueue->ExecuteCommandLists(_countof(lists), lists);
    }

    // 垂直同期によるPresent
    // パラメータ: SyncInterval=1（60 FPS でVsync有効）、Flags=0（標準Present）
    // 注意: バックバッファが D3D12_RESOURCE_STATE_PRESENT でなければ失敗します
    {
        ctx.swapChain->Present(1, 0);
    }

    // フレーム完了待機（GPU 処理完了までブロック）
    WaitForPreviousFrame();
}

// ========================================
// WaitForPreviousFrame
// ========================================
// 用途: GPU の処理完了を待機し、ダブルバッファインデックスを更新
// 処理:
//   1. フェンス値をインクリメント（GPU に通知）
//   2. GPU がそのフェンス値に達するまで待機（ブロック）
//   3. スワップチェーンの次のバックバッファインデックスを取得

// GPU 完了をフェンスで待機し、バックバッファインデックスを更新
void WaitForPreviousFrame()
{
    auto& ctx = GetD3D12Context();

    // GPU に現在のフェンス値を記録
    {
        const UINT64 currentFenceValue = ctx.fenceValue;
        ctx.commandQueue->Signal(ctx.fence.Get(), currentFenceValue);

        // GPU がそのフェンス値に達するまで待機（CPU ブロック）
        if (ctx.fence->GetCompletedValue() < currentFenceValue) {
            ctx.fence->SetEventOnCompletion(currentFenceValue, ctx.fenceEvent);
            WaitForSingleObject(ctx.fenceEvent, INFINITE);
        }
    }

    // 次フレーム用に フェンス値をインクリメント
    ++ctx.fenceValue;
    // スワップチェーンから次のバックバッファインデックスを取得（ダブルバッファリング）
    ctx.currentBackBufferIndex = ctx.swapChain->GetCurrentBackBufferIndex();
}
