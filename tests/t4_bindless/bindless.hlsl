// L5 bindless：98 个 SRV 的动态索引（对应语料统计的 p99=98 绑定槽）
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

cbuffer IdxCB : register(b0)
{
    uint g_idx;
};

Texture2D<float4> g_tex[98] : register(t0);
SamplerState      g_smp     : register(s0);

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 p[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
    VSOut o;
    o.pos = float4(p[vid], 0.0, 1.0);
    o.uv  = p[vid] * 0.5 + 0.5;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return g_tex[NonUniformResourceIndex(g_idx)].Sample(g_smp, i.uv);
}
