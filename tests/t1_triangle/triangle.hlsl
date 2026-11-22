// 最小三角形：顶点由 SV_VertexID 生成，不需要顶点缓冲/输入布局
struct VSOut
{
    float4 pos   : SV_Position;
    float3 color : COLOR;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 p[3] = { float2(-0.6, -0.5), float2(0.6, -0.5), float2(0.0, 0.6) };
    const float3 c[3] = { float3(1.0, 0.2, 0.2), float3(0.2, 1.0, 0.2), float3(0.2, 0.4, 1.0) };
    VSOut o;
    o.pos = float4(p[vid], 0.0, 1.0);
    o.color = c[vid];
    return o;
}

float4 PSMain(VSOut input) : SV_Target
{
    return float4(input.color, 1.0);
}
