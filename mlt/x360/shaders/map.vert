#version 450
// Um vertice do Dust II: posicao no mundo, coordenada da textura e a luz do sol ja calculada no PC.
layout(location = 0) in vec3 a_position;
layout(location = 2) in vec3 a_uv_light;
layout(location = 0) out vec2 v_uv;
// x: luz; y: quanto de nevoa
layout(location = 1) out vec2 v_light;
// mvp; eye.xyz, o olho; fog.x, onde a nevoa comeca; fog.y, 1 / (fim - comeco)
layout(push_constant) uniform Push { mat4 mvp; vec4 eye; vec4 fog; } pc;

void main()
{
	gl_Position = pc.mvp * vec4(a_position, 1.0);
	v_uv = a_uv_light.xy;
	float d = length(a_position - pc.eye.xyz);
	v_light = vec2(a_uv_light.z, clamp((d - pc.fog.x) * pc.fog.y, 0.0, 1.0));
}
