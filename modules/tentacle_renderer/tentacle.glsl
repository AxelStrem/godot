#[vertex]

#version 450

// *** Push constants (per-frame data) ***
layout(push_constant, std430) uniform PushConstants {
	uint segments;
	uint tentacle_count;
	float time;
	float noise_amplitude;
	float noise_freq;
	float noise_speed;
	float taper_amount;
	float width_scale;
	mat4 view_proj;
	vec3 cam_pos;
	float _pc_pad0;
}
pc;

// *** Outputs ***
layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) out float frag_edge_fade;

// *** Tentacle instance storage buffer (set=0, binding=0) ***
struct Tentacle {
	vec3 start;
	float _pad0;
	vec3 end;
	float _pad1;
	float progress;
	float thickness;
	float _pad2[2];
};

layout(set = 0, binding = 0, std430) readonly buffer TentacleBuffer {
	Tentacle tentacles[];
};

// ###########################################################################
// Simplex 3D noise (Ashima Arts, MIT license)
// ###########################################################################
vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 mod289(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 permute(vec4 x) { return mod289(((x*34.0)+1.0)*x); }
vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }

float simplex3d(vec3 v) {
	const vec2 C = vec2(1.0/6.0, 1.0/3.0);
	const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);

	vec3 i  = floor(v + dot(v, C.yyy));
	vec3 x0 = v - i + dot(i, C.xxx);

	vec3 g = step(x0.yzx, x0.xyz);
	vec3 l = 1.0 - g;
	vec3 i1 = min(g.xyz, l.zxy);
	vec3 i2 = max(g.xyz, l.zxy);

	vec3 x1 = x0 - i1 + C.xxx;
	vec3 x2 = x0 - i2 + C.yyy;
	vec3 x3 = x0 - D.yyy;

	i = mod289(i);
	vec4 p = permute(permute(permute(
		i.z + vec4(0.0, i1.z, i2.z, 1.0))
		+ i.y + vec4(0.0, i1.y, i2.y, 1.0))
		+ i.x + vec4(0.0, i1.x, i2.x, 1.0));

	float n_ = 0.142857142857;
	vec3  ns = n_ * D.wyz - D.xzx;

	vec4 j = p - 49.0 * floor(p * ns.z * ns.z);

	vec4 x_ = floor(j * ns.z);
	vec4 y_ = floor(j - 7.0 * x_);

	vec4 x = x_ *ns.x + ns.yyyy;
	vec4 y = y_ *ns.x + ns.yyyy;
	vec4 h = 1.0 - abs(x) - abs(y);

	vec4 b0 = vec4(x.xy, y.xy);
	vec4 b1 = vec4(x.zw, y.zw);

	vec4 s0 = floor(b0)*2.0 + 1.0;
	vec4 s1 = floor(b1)*2.0 + 1.0;
	vec4 sh = -step(h, vec4(0.0));

	vec4 a0 = b0.xzyw + s0.xzyw*sh.xxyy;
	vec4 a1 = b1.xzyw + s1.xzyw*sh.zzww;

	vec3 p0 = vec3(a0.xy, h.x);
	vec3 p1 = vec3(a0.zw, h.y);
	vec3 p2 = vec3(a1.xy, h.z);
	vec3 p3 = vec3(a1.zw, h.w);

	vec4 norm = taylorInvSqrt(vec4(dot(p0,p0), dot(p1,p1), dot(p2, p2), dot(p3,p3)));
	p0 *= norm.x;
	p1 *= norm.y;
	p2 *= norm.z;
	p3 *= norm.w;

	vec4 m = max(0.6 - vec4(dot(x0,x0), dot(x1,x1), dot(x2,x2), dot(x3,x3)), 0.0);
	m = m*m;
	return 42.0 * dot(m*m, vec4(dot(p0,x0), dot(p1,x1), dot(p2,x2), dot(p3,x3)));
}

// ###########################################################################
// Main
// ###########################################################################
void main() {
	uint tentacle_id = gl_InstanceIndex;
	uint seg = gl_VertexIndex / 2u;
	float side = (gl_VertexIndex % 2u == 0u) ? -1.0 : 1.0;

	// Cull segments beyond this tentacle's progress
	if (seg >= pc.segments || tentacle_id >= pc.tentacle_count) {
		gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
		frag_world_pos = vec3(0.0);
		frag_uv = vec2(0.0);
		frag_edge_fade = 0.0;
		return;
	}

	float t = float(seg) / float(max(pc.segments - 1u, 1u));

	Tentacle tcl = tentacles[tentacle_id];

	// Zero-width past the progress (makes area-zero triangles, GPU culls them).
	float progress = tcl.progress;
	if (t > progress) {
		t = progress;
		side = 0.0;
	}

	// Base straight-line position
	vec3 dir = normalize(tcl.end - tcl.start);
	vec3 raw_center = mix(tcl.start, tcl.end, t);

	// World-space noise wiggle, perpendicular to tentacle direction
	vec3 noise_in = raw_center * pc.noise_freq + vec3(pc.time * pc.noise_speed);
	vec3 cam_to_center = normalize(pc.cam_pos - raw_center);
	vec3 billboard_right = normalize(cross(dir, cam_to_center));
	float wiggle = simplex3d(noise_in) * pc.noise_amplitude;
	vec3 center = raw_center + billboard_right * wiggle;

	// Recompute billboard direction from the wiggled center
	cam_to_center = normalize(pc.cam_pos - center);
	billboard_right = normalize(cross(dir, cam_to_center));

	// Width with taper
	float taper = 1.0 - (1.0 - t) * pc.taper_amount;
	// When progress < 1, extra taper toward the tip
	taper *= 1.0 - pc.taper_amount * max(1.0 - progress, 0.0);
	float width = tcl.thickness * taper * pc.width_scale;

	vec3 vertex_pos = center + billboard_right * side * width;

	gl_Position = pc.view_proj * vec4(vertex_pos, 1.0);
	frag_world_pos = center;
	frag_uv = vec2(t, side * 0.5 + 0.5);
	frag_edge_fade = 1.0 - abs(side); // fade at edges
}

#[fragment]

#version 450

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in float frag_edge_fade;

layout(location = 0) out vec4 out_color;

void main() {
	// Edge-soften: darker near the edges, brighter in the center
	float edge = 1.0 - abs(frag_uv.y - 0.5) * 2.0;
	edge = smoothstep(0.0, 0.35, edge);

	// Core color: dark reddish-brown, brighter in center
	vec3 core = vec3(0.35, 0.12, 0.04);
	vec3 bright = vec3(0.55, 0.22, 0.08);
	vec3 color = mix(core, bright, edge);

	float alpha = edge * 0.85;

	out_color = vec4(color, alpha);
}
