#pragma once
// HLSL compiled at runtime with D3DCompile (d3dcompiler_47.dll ships with Windows 10/11).
// Shader model 5.0 -> any DX11 GPU (NVIDIA / AMD / Intel).

namespace BloodFX::Shaders
{
	inline constexpr const char* kCommon = R"HLSL(
struct Particle
{
	float3 pos;    float age;
	float3 vel;    float life;
	float3 normal; uint  state;   // 0 = dead, 1 = flying, 2 = stuck (splat)
};

struct Burst
{
	float3 pos;  uint  start;
	float3 dir;  uint  count;
	float speed; float spread; uint seed; float scale;
};

cbuffer Frame : register(b0)
{
	row_major float4x4 viewProj;      // relative to posAdjust
	row_major float4x4 invViewProj;
	float3 camPos;      float dt;
	float3 posAdjust;   uint  reversedZ;
	float2 screenSize;  float gravity;     float drag;
	uint   maxParticles; uint burstCount;  float stickSpeed; float bounce;
	float4 bloodColor;
	float  dropSize;    float splatSize;   float flyLife;    float splatLife;
	float  splatGrow;   float collideThickness; float time;  float pad0;
};

Texture2D<float>  SceneDepth : register(t0);
SamplerState      PointClamp : register(s0);

uint Hash(uint x)
{
	x ^= x >> 16; x *= 0x7feb352d;
	x ^= x >> 15; x *= 0x846ca68b;
	x ^= x >> 16; return x;
}
float Rand(inout uint s) { s = Hash(s); return (s & 0x00FFFFFF) / 16777216.0; }

float4 Project(float3 world) { return mul(viewProj, float4(world - posAdjust, 1)); }
bool   Behind(float pz, float sceneZ) { return reversedZ ? (pz < sceneZ) : (pz > sceneZ); }

float2 ClipToUV(float4 clip)
{
	float2 ndc = clip.xy / clip.w;
	return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float3 Unproject(float2 uv, float z)
{
	float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, z, 1);
	float4 w = mul(invViewProj, ndc);
	return w.xyz / w.w + posAdjust;
}

float3 SurfaceNormal(float2 uv, float3 center)
{
	float2 px = 1.0 / screenSize;
	float3 r = Unproject(uv + float2(px.x, 0), SceneDepth.SampleLevel(PointClamp, uv + float2(px.x, 0), 0));
	float3 d = Unproject(uv + float2(0, px.y), SceneDepth.SampleLevel(PointClamp, uv + float2(0, px.y), 0));
	float3 n = normalize(cross(r - center, d - center));
	if (dot(n, camPos - center) < 0) n = -n;
	return n;
}
)HLSL";

	// ------------------------------------------------------------------
	// Emit: one thread group row per burst. Writes into a ring buffer, so
	// no dead-list bookkeeping is needed; the oldest particles get recycled.
	// ------------------------------------------------------------------
	inline constexpr const char* kEmitCS = R"HLSL(
RWStructuredBuffer<Particle> P : register(u0);
StructuredBuffer<Burst> Bursts : register(t1);

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	Burst b = Bursts[gid.x];
	uint i = gid.y * 64 + tid.x;
	if (i >= b.count) return;

	uint s = b.seed ^ Hash(i * 9781);
	float3 dir = normalize(b.dir + (float3(Rand(s), Rand(s), Rand(s)) * 2 - 1) * b.spread);

	Particle p;
	p.pos    = b.pos + (float3(Rand(s), Rand(s), Rand(s)) * 2 - 1) * 3.0;
	p.vel    = dir * b.speed * lerp(0.35, 1.0, Rand(s));
	p.age    = 0;
	p.life   = flyLife * lerp(0.7, 1.3, Rand(s));
	p.normal = float3(0, 0, 1);
	p.state  = 1;
	P[(b.start + i) % maxParticles] = p;
}
)HLSL";

	// ------------------------------------------------------------------
	// Update: ballistic motion + collision against the on-screen depth buffer.
	// ------------------------------------------------------------------
	inline constexpr const char* kUpdateCS = R"HLSL(
RWStructuredBuffer<Particle> P : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= maxParticles) return;
	Particle p = P[id.x];
	if (p.state == 0) return;

	p.age += dt;
	if (p.age > p.life) { p.state = 0; P[id.x] = p; return; }
	if (p.state == 2)   { P[id.x] = p; return; }   // splats don't move

	p.vel.z -= gravity * dt;
	p.vel   *= saturate(1.0 - drag * dt);
	float3 next = p.pos + p.vel * dt;

	float4 clip = Project(next);
	if (clip.w > 0)
	{
		float2 uv = ClipToUV(clip);
		if (all(uv > 0.001) && all(uv < 0.999))
		{
			float sceneZ = SceneDepth.SampleLevel(PointClamp, uv, 0);
			float pz = clip.z / clip.w;
			if (Behind(pz, sceneZ))   // particle went behind the visible surface
			{
				float3 surf = Unproject(uv, sceneZ);
				float travel = length(p.vel) * dt;
				// Only a hit if we're near the surface (not merely occluded by something far in front).
				if (distance(next, surf) < collideThickness + travel)
				{
					float3 n  = SurfaceNormal(uv, surf);
					float  vn = dot(p.vel, n);
					if (length(p.vel) < stickSpeed || n.z > 0.5)
					{
						p.state  = 2;
						p.pos    = surf + n * 0.4;
						p.normal = n;
						p.vel    = 0;
						p.age    = 0;
						p.life   = splatLife;
						P[id.x]  = p;
						return;
					}
					p.vel = (p.vel - (1 + bounce) * vn * n) * 0.6;   // run down walls
					next  = surf + n * 0.5;
				}
			}
		}
	}
	p.pos = next;
	P[id.x] = p;
}
)HLSL";

	// ------------------------------------------------------------------
	// Render: no vertex buffer. 6 verts per particle from SV_VertexID.
	// Flying drops = velocity-stretched billboards. Stuck = decals on surface.
	// ------------------------------------------------------------------
	inline constexpr const char* kRenderVS = R"HLSL(
StructuredBuffer<Particle> P : register(t1);

struct VSOut
{
	float4 pos   : SV_Position;
	float2 uv    : TEXCOORD0;
	float3 world : TEXCOORD1;
	float  fade  : TEXCOORD2;
	nointerpolation uint kind : TEXCOORD3;
	nointerpolation uint seed : TEXCOORD4;
};

static const float2 kCorner[6] = { {-1,-1}, {1,-1}, {1,1}, {-1,-1}, {1,1}, {-1,1} };

VSOut main(uint vid : SV_VertexID)
{
	VSOut o = (VSOut)0;
	uint  pi = vid / 6;
	float2 c = kCorner[vid % 6];
	Particle p = P[pi];
	o.uv = c * 0.5 + 0.5;
	o.kind = p.state;
	o.seed = pi;

	if (p.state == 0) { o.pos = float4(2, 2, 2, 1); return o; }   // off-screen, culled

	float3 world;
	if (p.state == 1)
	{
		float3 toCam = normalize(camPos - p.pos);
		float3 v     = p.vel;
		float  speed = length(v);
		float3 axis  = speed > 1 ? v / speed : float3(0, 0, 1);
		float3 side  = normalize(cross(axis, toCam) + 1e-5);
		float  len   = dropSize + speed * 0.012;            // stretch with speed -> streaks
		world = p.pos + side * c.x * dropSize + axis * c.y * len;
		o.fade = 1.0;
	}
	else
	{
		float3 n = p.normal;
		float3 t = normalize(abs(n.z) < 0.9 ? cross(n, float3(0, 0, 1)) : cross(n, float3(1, 0, 0)));
		float3 b = cross(n, t);
		float  grow = splatSize * (1.0 + splatGrow * saturate(p.age / 2.0));   // pool spreads
		world = p.pos + (t * c.x + b * c.y) * grow;
		o.fade = 1.0 - saturate((p.age - (p.life - 5.0)) / 5.0);               // fade last 5s
	}

	o.world = world;
	o.pos   = Project(world);
	return o;
}
)HLSL";

	inline constexpr const char* kRenderPS = R"HLSL(
struct VSOut
{
	float4 pos   : SV_Position;
	float2 uv    : TEXCOORD0;
	float3 world : TEXCOORD1;
	float  fade  : TEXCOORD2;
	nointerpolation uint kind : TEXCOORD3;
	nointerpolation uint seed : TEXCOORD4;
};

float4 main(VSOut i) : SV_Target
{
	// Manual depth test against scene (we don't bind the game's DSV).
	float2 suv    = i.pos.xy / screenSize;
	float  sceneZ = SceneDepth.SampleLevel(PointClamp, suv, 0);
	float3 surf   = Unproject(suv, sceneZ);
	if (distance(camPos, i.world) > distance(camPos, surf) + 1.5) discard;

	float2 d = i.uv * 2 - 1;
	float  r;
	if (i.kind == 2)
	{
		// irregular splat edge
		float ang = atan2(d.y, d.x);
		uint  s   = Hash(i.seed);
		float wob = 0.12 * sin(ang * (3 + s % 4) + (s & 255)) + 0.06 * sin(ang * 9 + (s >> 8 & 255));
		r = length(d) / (0.85 + wob);
	}
	else r = length(d);

	float a = smoothstep(1.0, 0.6, r) * bloodColor.a * i.fade;
	if (a < 0.01) discard;

	// cheap "wet" look: darker core, small highlight
	float3 col  = bloodColor.rgb * lerp(0.55, 1.0, r);
	float  spec = smoothstep(0.25, 0.0, length(d - float2(-0.3, 0.35))) * (i.kind == 1 ? 0.5 : 0.25);
	col += spec;
	return float4(col * a, a);   // premultiplied alpha
}
)HLSL";
}
