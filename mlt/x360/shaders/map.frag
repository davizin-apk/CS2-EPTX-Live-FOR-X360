#version 450
// Textura vezes a cor do material vezes a luz do vertice, e a nevoa por cima.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_light;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push { vec4 color; vec4 fog_color; } pc;

void main()
{
	vec4 c = texture(u_texture, v_uv) * pc.color;
	vec3 lit = c.rgb * v_light.x;
	o_color = vec4(mix(lit, pc.fog_color.rgb, v_light.y), 1.0);
}
