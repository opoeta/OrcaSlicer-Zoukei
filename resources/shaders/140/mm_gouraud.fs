#version 140

#define INTENSITY_CORRECTION 0.6

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

#define INTENSITY_AMBIENT    0.3

const vec3  ZERO    = vec3(0.0, 0.0, 0.0);
const float EPSILON = 0.0001;
//BBS: add grey and orange
//const vec3 GREY = vec3(0.9, 0.9, 0.9);
const vec3 ORANGE = vec3(0.8, 0.4, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
uniform vec4 uniform_color;

uniform bool volume_mirrored;

uniform mat4 view_model_matrix;
uniform mat3 view_normal_matrix;

in vec3 clipping_planes_dots;
in vec4 model_pos;
in vec4 world_pos;

struct SlopeDetection
{
    bool actived;
	 float normal_z;
    mat3 volume_world_normal_matrix;
    vec3 up_direction;
};
uniform SlopeDetection slope;

out vec4 out_color;

//BBS: add wireframe logic
in vec3 barycentric_coordinates;
float edgeFactor(float lineWidth) {
    vec3 d = fwidth(barycentric_coordinates);
    vec3 a3 = smoothstep(vec3(0.0), d * lineWidth, barycentric_coordinates);
    return min(min(a3.x, a3.y), a3.z);
}

vec3 wireframe(vec3 fill, vec3 stroke, float lineWidth) {
    return mix(stroke, fill, edgeFactor(lineWidth));
	//if (any(lessThan(barycentric_coordinates, vec3(0.005, 0.005, 0.005))))
	//	return vec3(1.0, 0.0, 0.0);
	//else
	//	return fill;
}

vec3 getWireframeColor(vec3 fill) {
    float brightness = 0.2126 * fill.r + 0.7152 * fill.g + 0.0722 * fill.b;
    return (brightness > 0.75) ? vec3(0.11, 0.165, 0.208) : vec3(0.988, 0.988, 0.988);
}
uniform bool show_wireframe;

// ORCA: realistic view (after PrusaSlicer 3.0): key and fill lights, dielectric material (roughness 0.5,
// IOR 1.5) with Cook-Torrance GGX in linear light, Reinhard tone mapped with a white point that keeps
// colours recognisable, plus subtle printed layer lines. realistic_view < 0.5 keeps the classic look.
uniform float realistic_view;
uniform vec3 realistic_up_eye;
uniform vec2 layer_lines; // x = layer height (<= 0 disables), y = z of the first layer
const vec3 RV_FILL_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
float rv_ggx_g(float c) { float k = 2.25 / 8.0; return c / (c * (1.0 - k) + k); }
vec3 rv_light(vec3 base, vec3 n, vec3 v, vec3 l, float intensity) {
    float n_dot_l = max(dot(n, l), 0.0);
    if (n_dot_l <= 0.0)
        return vec3(0.0);
    vec3 h = normalize(v + l);
    float n_dot_v = max(dot(n, v), 0.0);
    float f = 0.04 + 0.96 * pow(clamp(1.0 - dot(h, v), 0.0, 1.0), 5.0);
    float a2 = 0.0625;
    float d = max(dot(n, h), 0.0);
    d = d * d * (a2 - 1.0) + 1.0;
    float spec = (a2 / (3.14159265 * d * d)) * rv_ggx_g(n_dot_v) * rv_ggx_g(n_dot_l) * f / (4.0 * n_dot_v * n_dot_l + 1e-4);
    return ((1.0 - f) * base / 3.14159265 + vec3(spec)) * intensity * n_dot_l;
}
vec3 realistic_lit(vec3 base_srgb, vec3 normal, vec3 key_dir, float world_z, float shade) {
    vec3 n = normalize(normal);
    vec3 base = pow(clamp(base_srgb, 0.0, 1.0), vec3(2.2));
    float ao = 1.0;
    if (layer_lines.x > 0.0) {
        float up = dot(n, realistic_up_eye);
        float wall = 1.0 - up * up;
        float f = fract((world_z - layer_lines.y) / layer_lines.x);
        ao = 1.0 - 0.10 * wall * smoothstep(0.35, 1.0, f);
    }
    vec3 v = vec3(0.0, 0.0, 1.0);
    vec3 c = 0.5 * ao * base + ao * (shade * rv_light(base, n, v, key_dir, 9.6) + rv_light(base, n, v, RV_FILL_DIR, 3.6));
    c = c * (1.0 + c / 16.0) / (1.0 + c);
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;
    vec3  color = uniform_color.rgb;
    float alpha = uniform_color.a;

    vec3 triangle_normal = normalize(cross(dFdx(model_pos.xyz), dFdy(model_pos.xyz)));
#ifdef FLIP_TRIANGLE_NORMALS
    triangle_normal = -triangle_normal;
#endif

    if (volume_mirrored)
        triangle_normal = -triangle_normal;

    vec3 transformed_normal = normalize(slope.volume_world_normal_matrix * triangle_normal);
     
    if (slope.actived) {
        if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
              color = LightBlue;
              alpha = 1.0;
         }
         else if( dot(transformed_normal, slope.up_direction) < slope.normal_z - EPSILON)
        {
            color = color * 0.5 + LightRed * 0.5;
            alpha = 1.0;
        }
    }
    // First transform the normal into camera space and normalize the result.
    vec3 eye_normal = normalize(view_normal_matrix * triangle_normal);

    // Compute the cos of the angle between the normal and lights direction. The light is directional so the direction is constant for every vertex.
    // Since these two are normalized the cosine is the dot product. We also need to clamp the result to the [0,1] range.
    float NdotL = max(dot(eye_normal, LIGHT_TOP_DIR), 0.0);

    // x = diffuse, y = specular;
    vec2 intensity = vec2(0.0);
    intensity.x = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    vec3 position = (view_model_matrix * model_pos).xyz;
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position), reflect(-LIGHT_TOP_DIR, eye_normal)), 0.0), LIGHT_TOP_SHININESS);

    // Perform the same lighting calculation for the 2nd light source (no specular applied).
    NdotL = max(dot(eye_normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

    if (show_wireframe) {
        vec3 wireframeColor = show_wireframe ? getWireframeColor(color) : color;
        vec3 triangleColor = wireframe(color, wireframeColor, 1.0);
        out_color = realistic_view > 0.5 ? vec4(realistic_lit(triangleColor, eye_normal, LIGHT_TOP_DIR, world_pos.z, 1.0), alpha) : vec4(vec3(intensity.y) + triangleColor * intensity.x, alpha);
    }
    else {
        out_color = realistic_view > 0.5 ? vec4(realistic_lit(color, eye_normal, LIGHT_TOP_DIR, world_pos.z, 1.0), alpha) : vec4(vec3(intensity.y) + color * intensity.x, alpha);
    }
}
