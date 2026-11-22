// L4 纹理采样 + 常量缓冲
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

cbuffer Tint : register(b0)
{
    float4 g_tint;
};

Texture2D<float4> g_tex : register(t0);
SamplerState      g_smp : register(s0);

VSOut VSMain(uint vid : SV_VertexID)
{
    // triangle strip: 4 个顶点覆盖 -0.8..0.8
    const float2 p[4] = { float2(-0.8, -0.8), float2(0.8, -0.8), float2(-0.8, 0.8), float2(0.8, 0.8) };
    VSOut o;
    o.pos = float4(p[vid], 0.0, 1.0);
    o.uv  = p[vid] * 0.5 + 0.5;
    return o;
}

float4 PSMain(VSOut input) : SV_Target
{
    return g_tex.Sample(g_smp, input.uv) * g_tint;
}
