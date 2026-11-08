#version 450
// Um vertice de modelo (agente, arma, peca de construcao): posicao, normal e textura; a luz do sol por vertice.
// A saida e a mesma do map.vert, para usar o map.frag e o map_alpha.frag.
layout(location = 0) in vec3 a_position;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec3 a_normal;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_light;
// mvp; as tres linhas da matriz do modelo; o olho; a nevoa (comeco, 1/(fim-comeco)); a luz (direcao do sol em xyz,
// ambiente em w); luz2 (forca do sol em x; y = 1 sem luz)
layout(push_constant) uniform Push { mat4 mvp; vec4 row0; vec4 row1; vec4 row2; vec4 eye; vec4 fog; vec4 light; vec4 light2; } pc;

void main()
{
	vec4 p = vec4(a_position, 1.0);
	gl_Position = pc.mvp * p;
	vec3 world = vec3(dot(pc.row0, p), dot(pc.row1, p), dot(pc.row2, p));
	vec3 n = normalize(vec3(dot(pc.row0.xyz, a_normal), dot(pc.row1.xyz, a_normal), dot(pc.row2.xyz, a_normal)));
	float lit = pc.light.w + pc.light2.x * max(dot(n, -pc.light.xyz), 0.0);
	lit = mix(lit, 1.0, pc.light2.y);
	float d = length(world - pc.eye.xyz);
	v_uv = a_uv;
	v_light = vec2(lit, clamp((d - pc.fog.x) * pc.fog.y, 0.0, 1.0));
}
