#pragma once

struct D3D12Context; // forward declaration

// Initializes triangle sample pipeline (root signature, PSO, vertex buffer)
void InitializeTrianglePipeline(D3D12Context& ctx);
