#include "DirectXMain.h"
#include "DirectX12TriangleSample.h"
#include <d3dcompiler.h>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

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
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&ctx.device)))) break;
    }
    if (!ctx.device) throw std::runtime_error("Failed to create D3D12 device");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ctx.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&ctx.commandQueue))))
        throw std::runtime_error("Failed to create command queue");

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(ctx.commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain)))
        throw std::runtime_error("Failed to create swap chain");
    if (FAILED(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER)))
        throw std::runtime_error("Failed to make window association");
    swapChain.As(&ctx.swapChain);
    ctx.frameIndex = ctx.swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(ctx.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&ctx.rtvHeap))))
        throw std::runtime_error("Failed to create RTV heap");
    ctx.rtvDescriptorSize = ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(ctx.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < FRAME_COUNT; ++i) {
        if (FAILED(ctx.swapChain->GetBuffer(i, IID_PPV_ARGS(&ctx.renderTargets[i]))))
            throw std::runtime_error("Failed to get back buffer");
        ctx.device->CreateRenderTargetView(ctx.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += ctx.rtvDescriptorSize;
    }
    if (FAILED(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx.commandAllocator))))
        throw std::runtime_error("Failed to create command allocator");
    if (FAILED(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx.commandAllocator.Get(), nullptr, IID_PPV_ARGS(&ctx.commandList))))
        throw std::runtime_error("Failed to create command list");
    ctx.commandList->Close();
    if (FAILED(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx.fence))))
        throw std::runtime_error("Failed to create fence");
    ctx.fenceValue = 1;
    ctx.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!ctx.fenceEvent) throw std::runtime_error("Failed to create fence event");

    InitializeTrianglePipeline(ctx);
}

void Update() {}

void Render()
{
    auto& ctx = GetD3D12Context();
    ctx.commandAllocator->Reset();
    ctx.commandList->Reset(ctx.commandAllocator.Get(), ctx.pipelineState.Get());

    D3D12_RESOURCE_DESC backBufferDesc = ctx.renderTargets[ctx.frameIndex]->GetDesc();
    D3D12_VIEWPORT viewport{0.0f,0.0f, static_cast<float>(backBufferDesc.Width), static_cast<float>(backBufferDesc.Height), 0.0f,1.0f};
    ctx.commandList->RSSetViewports(1,&viewport);
    RECT scissor{0,0,(LONG)backBufferDesc.Width,(LONG)backBufferDesc.Height};
    ctx.commandList->RSSetScissorRects(1,&scissor);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = ctx.renderTargets[ctx.frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ctx.commandList->ResourceBarrier(1,&barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(ctx.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvHandle.ptr += (SIZE_T)ctx.frameIndex * ctx.rtvDescriptorSize;
    ctx.commandList->OMSetRenderTargets(1,&rtvHandle,FALSE,nullptr);

    const float clearColor[] = {0.39f,0.58f,0.93f,1.0f};
    ctx.commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    ctx.commandList->SetGraphicsRootSignature(ctx.rootSignature.Get());
    ctx.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx.commandList->IASetVertexBuffers(0,1,&ctx.vertexBufferView);
    ctx.commandList->DrawInstanced(3,1,0,0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    ctx.commandList->ResourceBarrier(1,&barrier);

    ctx.commandList->Close();
    ID3D12CommandList* lists[] = { ctx.commandList.Get() };
    ctx.commandQueue->ExecuteCommandLists(_countof(lists), lists);
    ctx.swapChain->Present(1,0);
    WaitForPreviousFrame();
}

void WaitForPreviousFrame()
{
    auto& ctx = GetD3D12Context();
    const UINT64 currentFenceValue = ctx.fenceValue;
    ctx.commandQueue->Signal(ctx.fence.Get(), currentFenceValue);
    ctx.fenceValue++;
    if (ctx.fence->GetCompletedValue() < currentFenceValue) {
        ctx.fence->SetEventOnCompletion(currentFenceValue, ctx.fenceEvent);
        WaitForSingleObject(ctx.fenceEvent, INFINITE);
    }
    ctx.frameIndex = ctx.swapChain->GetCurrentBackBufferIndex();
}

void CleanupD3D12()
{
    auto& ctx = GetD3D12Context();
    if (ctx.fenceEvent) { CloseHandle(ctx.fenceEvent); ctx.fenceEvent = nullptr; }
}
