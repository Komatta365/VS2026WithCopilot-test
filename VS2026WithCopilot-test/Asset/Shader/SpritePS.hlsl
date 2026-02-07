Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);
struct PSInput { float4 pos : SV_Position; float2 uv : TEXCOORD; };
float4 PSMain(PSInput input) : SV_Target
{
    return gTexture.Sample(gSampler, input.uv);
}
