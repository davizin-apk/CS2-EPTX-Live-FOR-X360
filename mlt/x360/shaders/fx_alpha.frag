#version 450
// O fx.frag com recorte por alfa (marcas de tiro, particulas, o oval do portal).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main()
{
	vec4 c = texture(u_texture, v_uv) * v_color;
	if (c.a < 0.5)
		discard;
	o_color = vec4(c.rgb, 1.0);
}
