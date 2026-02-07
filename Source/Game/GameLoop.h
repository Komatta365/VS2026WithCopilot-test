#pragma once
#include <DirectXMath.h>
#include <vector>
#include <algorithm>

using namespace DirectX;

struct Bullet {
    XMFLOAT3 position;
    bool active;
};

struct Enemy {
    XMFLOAT3 position;
    bool active;
};

struct Building {
    XMFLOAT3 position;
    XMFLOAT3 scale;
};

struct GameState {
    XMFLOAT3 playerPos{ 0.0f, 0.0f, -5.0f };
    std::vector<Bullet> bullets;
    std::vector<Enemy> enemies;
    std::vector<Building> buildings;
    float enemySpawnTimer = 0.0f;
    float gameTime = 0.0f;
};

inline GameState g_gameState;

inline void InitializeGame() {
    // Generate random buildings
    for (int i = 0; i < 50; ++i) {
        float x = (rand() % 100) - 50.0f;
        // Keep center clear for gameplay
        if (x > -15.0f && x < 15.0f) {
            if (x < 0) x -= 20.0f; else x += 20.0f;
        }
        float z = (rand() % 200) - 50.0f;
        float h = (rand() % 20) + 5.0f;
        float w = (rand() % 5) + 2.0f;
        float d = (rand() % 5) + 2.0f;
        g_gameState.buildings.push_back({ { x, h * 0.5f - 2.0f, z }, { w, h, d } });
    }
}

inline void UpdateGame(float deltaSeconds) {
    static bool initialized = false;
    if (!initialized) {
        InitializeGame();
        initialized = true;
    }

    g_gameState.gameTime += deltaSeconds;

    // Player movement
    const float speed = 10.0f;
    // Auto-advance forward
    g_gameState.playerPos.z += speed * deltaSeconds;

    // Arrow keys for X/Y movement
    if (GetAsyncKeyState(VK_UP) & 0x8000) g_gameState.playerPos.y += speed * deltaSeconds;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) g_gameState.playerPos.y -= speed * deltaSeconds;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) g_gameState.playerPos.x -= speed * deltaSeconds;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) g_gameState.playerPos.x += speed * deltaSeconds;

    // Clamp Y to avoid going underground
    if (g_gameState.playerPos.y < 1.0f) g_gameState.playerPos.y = 1.0f;

    // Shooting
    static bool spacePressed = false;
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
        if (!spacePressed) {
            g_gameState.bullets.push_back({ g_gameState.playerPos, true });
            spacePressed = true;
        }
    } else {
        spacePressed = false;
    }

    // Update bullets
    for (auto& b : g_gameState.bullets) {
        if (b.active) {
            b.position.z += 20.0f * deltaSeconds;
            if (b.position.z > g_gameState.playerPos.z + 50.0f) b.active = false;
        }
    }

    // Recycle buildings
    for (auto& b : g_gameState.buildings) {
        if (b.position.z < g_gameState.playerPos.z - 20.0f) {
            b.position.z += 200.0f;
        }
    }

    // Spawn enemies
    g_gameState.enemySpawnTimer += deltaSeconds;
    if (g_gameState.enemySpawnTimer > 2.0f) {
        g_gameState.enemySpawnTimer = 0.0f;
        float x = (rand() % 20) - 10.0f;
        g_gameState.enemies.push_back({ { x, 0.0f, g_gameState.playerPos.z + 40.0f }, true });
    }

    // Collision
    for (auto& b : g_gameState.bullets) {
        if (!b.active) continue;
        for (auto& e : g_gameState.enemies) {
            if (!e.active) continue;
            float dx = b.position.x - e.position.x;
            float dz = b.position.z - e.position.z;
            if (sqrt(dx*dx + dz*dz) < 1.5f) {
                b.active = false;
                e.active = false;
            }
        }
    }

    // Cleanup
    g_gameState.bullets.erase(std::remove_if(g_gameState.bullets.begin(), g_gameState.bullets.end(), [](const Bullet& b){ return !b.active; }), g_gameState.bullets.end());
    g_gameState.enemies.erase(std::remove_if(g_gameState.enemies.begin(), g_gameState.enemies.end(), [](const Enemy& e){ return !e.active; }), g_gameState.enemies.end());
}
