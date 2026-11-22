// L6 mesh shader：对应语料里 34.8% 的 NGG 着色器
struct VSOut
{
    float4 pos   : SV_Position;
    float3 color : COLOR;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void MSMain(out vertices VSOut verts[3], out indices uint3 tris[1])
{
    SetMeshOutputCounts(3, 1);

    verts[0].pos = float4(-0.8, -0.7, 0.0, 1.0);
    verts[0].color = float3(1.0, 0.2, 0.2);

    verts[1].pos = float4(0.8, -0.7, 0.0, 1.0);
    verts[1].color = float3(0.2, 1.0, 0.2);

    verts[2].pos = float4(0.0, 0.8, 0.0, 1.0);
    verts[2].color = float3(0.2, 0.4, 1.0);

    tris[0] = uint3(0, 1, 2);
}

float4 PSMain(VSOut input) : SV_Target
{
    return float4(input.color, 1.0);
}
