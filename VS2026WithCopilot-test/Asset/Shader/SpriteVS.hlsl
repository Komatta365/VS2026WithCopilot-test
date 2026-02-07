cbuffer SpriteCB : register(b0)
{
    float4x4 gTransform;
}
struct VSInput { float3 pos : POSITION; float2 uv : TEXCOORD; };
struct PSInput { float4 pos : SV_Position; float2 uv : TEXCOORD; };
PSInput VSMain(VSInput input)
{
    PSInput o;
    o.pos = mul(float4(input.pos, 1.0f), gTransform);
    o.uv = input.uv;
    return o;
}
