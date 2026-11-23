// ui.hlsl —— Xbox 风格 shell 的 UI 着色器
//
// 设计取向：**不用输入布局**。本源化项目里所有已验证的测试都用 SV_VertexID
// 生成几何，从没走过 D3D12 输入布局（它是 PSO 创建失败的一个隐蔽原因）。
// 这里沿用同一条路：
//   - 顶点数据放 StructuredBuffer（t0），按 SV_VertexID 取
//   - 坐标在 CPU 端就换算成 NDC（-1..1），不需要 cbuffer
//   - 纹理图集在 t1（文字由 GDI 渲染进去），静态采样器 s0
// 每个顶点带 UV 与颜色：纯色块把 UV 指到图集里的白色像素，乘顶点色即可。
struct GpuVertex {
    float2 pos;    // 已经是 NDC
    float2 uv;
    float4 col;
};

StructuredBuffer<GpuVertex> g_verts : register(t0);
Texture2D atlas : register(t1);
SamplerState samp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    GpuVertex v = g_verts[vid];
    VSOut o;
    o.pos = float4(v.pos, 0.0, 1.0);
    o.uv = v.uv;
    o.col = v.col;
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float4 t = atlas.Sample(samp, i.uv);
    return float4(t.rgb * i.col.rgb, t.a * i.col.a);
}
