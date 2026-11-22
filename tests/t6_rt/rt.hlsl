// t6_rt — L7：DXR 1.0 最小光追管线
RWTexture2D<float4> g_out  : register(u0);
RaytracingAccelerationStructure g_tlas : register(t0);

struct Payload
{
    float4 color;
};

[shader("raygeneration")]
void RayGen()
{
    uint2 idx = DispatchRaysIndex().xy;
    float2 ndc = (float2(idx) + 0.5) / float2(64.0, 64.0) * 2.0 - 1.0;

    RayDesc ray;
    ray.Origin    = float3(0.0, 0.0, -2.0);
    ray.Direction = normalize(float3(ndc.x, ndc.y, 1.0));
    ray.TMin      = 0.001;
    ray.TMax      = 1000.0;

    Payload p;
    p.color = float4(0.0, 0.0, 1.0, 1.0);          // miss -> 蓝
    TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);
    g_out[idx] = p.color;
}

[shader("miss")]
void Miss(inout Payload p)
{
    p.color = float4(0.0, 0.0, 1.0, 1.0);          // 明确声明 miss 颜色
}

[shader("closesthit")]
void ClosestHit(inout Payload p, in BuiltInTriangleIntersectionAttributes attr)
{
    p.color = float4(0.0, 1.0, 0.0, 1.0);          // hit -> 绿
}
