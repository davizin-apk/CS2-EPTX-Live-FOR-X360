#version 450
// Efeitos (tracos, brilho do cano, particulas, hologramas, portais) e a HUD: posicao, cor e textura.
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(push_constant) uniform Push { mat4 mvp; } pc;

void main()
{
	gl_Position = pc.mvp * vec4(a_position, 1.0);
	v_uv = a_uv;
	v_color = a_color;
}
