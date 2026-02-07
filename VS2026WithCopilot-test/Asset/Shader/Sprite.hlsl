// Textured sprite shader (VS/PS)
// Input layout: POSITION (float3), TEXCOORD (float2)
// Constant buffer: transform (float4x4)

cbuffer SpriteCB : register(b0)
{
    float4x4 gTransform;
}

struct VSInput
{
    float3 pos : POSITION;
    float2 uv  : TEXCOORD;
};

struct PSInput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    o.pos = mul(float4(input.pos, 1.0f), gTransform);
    o.uv = input.uv;
    return o;
}

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

float4 PSMain(PSInput input) : SV_Target
{
    return gTexture.Sample(gSampler, input.uv);
}
