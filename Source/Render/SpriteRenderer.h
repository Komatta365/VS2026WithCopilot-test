#pragma once
#include "DirectXMain.h"

// Initializes textured sprite pipeline (root signature, PSO, resources)
void InitializeSpritePipeline(D3D12Context& ctx);

// Records draw commands for a full-screen sprite
void DrawSprite(D3D12Context& ctx);
