// t8 模型查看器：MVP 由 CPU 算好（行向量约定 -> mul(vec, M)）
// 三种编译变体：
//   默认        正常渲染（CBV 里的 g_mvp）
//   -D DBG_VB   绕过 MVP，把顶点位置直接放大 + uv 当颜色（验证顶点数据是否到达 GPU）
//   -D DBG_HARDMVP 用硬编码矩阵（验证 CBV 是否是问题源）
cbuffer Cam : register(b0)
{
    // HLSL 常量缓冲默认按**列主序**解释矩阵；我们 CPU 侧是行主序，
    // 不加 row_major 会被当成转置矩阵用 —— 结果顶点全塌到一个点，画面全空。
    row_major float4x4 g_mvp;
};

Texture2D<float4> g_tex : register(t0);
SamplerState      g_smp : register(s0);

struct VSIn
{
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float3 nrm : NORMAL;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float3 nrm : TEXCOORD1;
};

VSOut VSMain(VSIn input)
{
    VSOut o;
#if defined(DBG_HARDMVP)
    static const float4x4 H = { 1.4, 0, 0, 0,
                                0, 1.4, 0, 0,
                                0, 0, 1.0, 0,
                                0, 0, 0.5, 1 };
    o.pos = mul(float4(input.pos, 1.0), H);
#elif defined(DBG_VB)
    o.pos = float4(input.pos.x * 3.0, input.pos.y * 3.0, 0.0, 1.0);
#else
    o.pos = mul(float4(input.pos, 1.0), g_mvp);
#endif
    o.uv  = input.uv;
    o.nrm = input.nrm;
    return o;
}

float4 PSMain(VSOut input) : SV_Target
{
#if defined(DBG_HARDMVP) || defined(DBG_VB)
    return float4(input.uv, 0.0, 1.0);
#else
    float4 c = g_tex.Sample(g_smp, input.uv);
    // 半兰伯特：固定光方向，避免无光照时看不出体积
    const float3 L = normalize(float3(0.35, 0.75, 0.55));
    const float ndl = saturate(dot(normalize(input.nrm), L));
    // 提亮：很多占位贴图本身很暗，配合 0.45 的底光会糊成黑块
    const float shade = 0.55 + 0.65 * ndl;
    return float4(saturate(c.rgb * shade * 1.6), 1.0);
#endif
}
