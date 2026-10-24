#version 110

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
//BBS: add grey and orange
//const vec3 GREY = vec3(0.9, 0.9, 0.9);
const vec3 ORANGE = vec3(0.8, 0.4, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
const float EPSILON = 0.0001;

struct PrintVolumeDetection
{
	// 0 = rectangle, 1 = circle, 2 = custom, 3 = invalid
	int type;
    // type = 0 (rectangle):
    // x = min.x, y = min.y, z = max.x, w = max.y
    // type = 1 (circle):
    // x = center.x, y = center.y, z = radius
	vec4 xy_data;
    // x = min z, y = max z
	vec2 z_data;
};

struct SlopeDetection
{
    bool actived;
	float normal_z;
    mat3 volume_world_normal_matrix;
    vec3 up_direction;
};

uniform vec4 uniform_color;
uniform bool use_color_clip_plane;
uniform vec4 uniform_color_clip_plane_1;
uniform vec4 uniform_color_clip_plane_2;
uniform SlopeDetection slope;

//BBS: add outline_color
uniform bool is_outline;
uniform sampler2D depth_tex;
uniform vec2 screen_size;


#ifdef ENABLE_ENVIRONMENT_MAP
    uniform sampler2D environment_tex;
    uniform bool use_environment_tex;
#endif // ENABLE_ENVIRONMENT_MAP

uniform PrintVolumeDetection print_volume;
// BBS H2D/H2C per-extruder printable height (3DScene.cpp): .x = flag (>=1 active), .y/.z = the two
// extruders' Z limits. Inert unless the CPU sets .x >= 1.0 (multi-extruder printers only), so the
// shared object shader stays pixel-identical for single-extruder printers. See 3DScene.cpp.
uniform vec3 extruder_printable_heights;
const float ONE_OVER_EPSILON = 1e4;

uniform float z_far;
uniform float z_near;

// Depth-based shadow map (object-on-object and self shadows). shadow_intensity == 0 disables it.
uniform sampler2D shadow_map;
uniform mat4 shadow_light_vp;
uniform float shadow_intensity;
uniform float shadow_map_texel;

// LIGHT_TOP_DIR in eye space (matches the diffuse light used for shading in gouraud.vs).
const vec3 SHADOW_LIGHT_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
// ORCA: realistic view - static shadows also light the scene from their fixed light.
uniform bool use_static_light;
uniform vec3 static_light_dir;
vec3 top_light_dir() { return use_static_light ? static_light_dir : SHADOW_LIGHT_DIR; }

varying vec3 clipping_planes_dots;
varying float color_clip_plane_dot;

// x = diffuse, y = specular;
varying vec2 intensity;

varying vec4 world_pos;
varying float world_normal_z;
varying vec3 eye_normal;

vec3 getBackfaceColor(vec3 fill) {
    float brightness = 0.2126 * fill.r + 0.7152 * fill.g + 0.0722 * fill.b;
    return (brightness > 0.75) ? vec3(0.11, 0.165, 0.208) : vec3(0.988, 0.988, 0.988);
}

// Silhouette edge detection & rendering algorithem by leoneruggiero
// https://www.shadertoy.com/view/DslXz2
#define INFLATE 1

float GetTolerance(float d, float k)
{
    // -------------------------------------------
    // Find a tolerance for depth that is constant
    // in view space (k in view space).
    //
    // tol = k*ddx(ZtoDepth(z))
    // -------------------------------------------
    
    float A=-   (z_far+z_near)/(z_far-z_near);
    float B=-2.0*z_far*z_near /(z_far-z_near);
    
    d = d*2.0-1.0;
    
    return -k*(d+A)*(d+A)/B;   
}

float DetectSilho(vec2 fragCoord, vec2 dir)
{
    // -------------------------------------------
    //   x0 ___ x1----o 
    //          :\    : 
    //       r0 : \   : r1
    //          :  \  : 
    //          o---x2 ___ x3
    //
    // r0 and r1 are the differences between actual
    // and expected (as if x0..3 where on the same
    // plane) depth values.
    // -------------------------------------------
    
    float x0 = abs(texture2D(depth_tex, (fragCoord + dir*-2.0) / screen_size).r);
    float x1 = abs(texture2D(depth_tex, (fragCoord + dir*-1.0) / screen_size).r);
    float x2 = abs(texture2D(depth_tex, (fragCoord + dir* 0.0) / screen_size).r);
    float x3 = abs(texture2D(depth_tex, (fragCoord + dir* 1.0) / screen_size).r);
    
    float d0 = (x1-x0);
    float d1 = (x2-x3);
    
    float r0 = x1 + d0 - x2;
    float r1 = x2 + d1 - x1;
    
    float tol = GetTolerance(x2, 0.04);
    
    return smoothstep(0.0, tol*tol, max( - r0*r1, 0.0));

}

float DetectSilho(vec2 fragCoord)
{
    return max(
        DetectSilho(fragCoord, vec2(1,0)), // Horizontal
        DetectSilho(fragCoord, vec2(0,1))  // Vertical
        );
}

// Returns a lighting multiplier in [1 - shadow_intensity, 1]: < 1 where the fragment is
// occluded from the light in the shadow map. 3x3 PCF softens the edges.
float shadow_shade()
{
    if (shadow_intensity <= 0.0)
        return 1.0;

    vec4 lp = shadow_light_vp * world_pos;
    vec3 proj = lp.xyz / lp.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0)
        return 1.0;

    // Slope-scaled depth bias: larger where the surface grazes / faces away from the light. This
    // suppresses self-shadow acne without discarding real shadows cast by other objects onto
    // back-facing surfaces (e.g. the shaded back/tip of a cone sitting inside a larger shadow).
    float NdotL = dot(normalize(eye_normal), top_light_dir());
    float bias = mix(0.0004, 0.004, clamp(1.0 - NdotL, 0.0, 1.0));
    // 5x5 PCF: softens shadow edges into a smooth penumbra and blurs residual facet acne.
    float sum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float closest = texture2D(shadow_map, proj.xy + vec2(float(x), float(y)) * shadow_map_texel).r;
            sum += (proj.z - bias > closest) ? 1.0 : 0.0;
        }
    }
    return 1.0 - shadow_intensity * (sum / 25.0);
}

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

varying vec3 mc_normal;

// Orca Zoukei: multi-colour filaments in the Prepare view (see GUI/Zoukei/PrepareMultiColor.cpp).
// mc_mode: 0 = off (the volume keeps uniform_color), 1 = coaxial (colour by the side the surface faces,
// thin stripes on the top and bottom faces), 2 = gradient (colours cycle with world Z, mc_cycle mm per cycle).
// An approximation of the print, not a simulation of it.
uniform int mc_mode;
uniform int mc_count;
uniform float mc_cycle;
uniform vec4 mc_c0;
uniform vec4 mc_c1;
uniform vec4 mc_c2;
uniform vec4 mc_c3;

vec3 zk_mc_get(int i)
{
    int n = mc_count < 1 ? 1 : mc_count;
    if (i < 0)
        i += n;
    if (i >= n)
        i -= n;
    if (i == 1)
        return mc_c1.rgb;
    if (i == 2)
        return mc_c2.rgb;
    if (i == 3)
        return mc_c3.rgb;
    return mc_c0.rgb;
}

vec3 zk_multicolor(vec3 normal, vec3 wp)
{
    float n = float(mc_count < 1 ? 1 : mc_count);

    if (mc_mode == 2) {
        float t = fract(wp.z / max(mc_cycle, 1.0)) * n;
        float fi = floor(t);
        return mix(zk_mc_get(int(fi)), zk_mc_get(int(fi) + 1), smoothstep(0.0, 1.0, t - fi));
    }

    vec3 nn = normalize(normal);

    // Side walls: the azimuth of the normal is split into mc_count equal sectors, with a soft edge.
    float s = 0.0;
    if (length(nn.xy) > 0.0001)
        s = (atan(nn.y, nn.x) / 6.2831853 + 0.5) * n;
    float k = floor(s);
    float f = s - k;
    int ki = int(k);
    vec3 side = zk_mc_get(ki);
    const float W = 0.06;
    if (f < W)
        side = mix(zk_mc_get(ki - 1), side, 0.5 + 0.5 * f / W);
    else if (f > 1.0 - W)
        side = mix(side, zk_mc_get(ki + 1), 0.5 * (f - (1.0 - W)) / W);

    // Top and bottom faces: thin parallel stripes (2 mm each), fading to the average colour when they
    // get smaller than a pixel. The derivatives are taken outside any branch.
    float q = (wp.x + wp.y) * 0.35355339;
    float fw = max(abs(dFdx(q)) + abs(dFdy(q)), 0.0001);
    float qi = floor(q);
    int si = int(mod(qi, n));
    vec3 stripe = mix(zk_mc_get(si - 1), zk_mc_get(si), clamp((q - qi) / fw, 0.0, 1.0));
    vec3 avg = vec3(0.0);
    for (int i = 0; i < 4; ++i)
        if (i < mc_count)
            avg += zk_mc_get(i);
    avg /= n;
    stripe = mix(stripe, avg, smoothstep(0.3, 0.7, fw));

    return mix(side, stripe, smoothstep(0.55, 0.8, abs(nn.z)));
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    vec4 color;
	if (use_color_clip_plane) {
		color.rgb = (color_clip_plane_dot < 0.0) ? uniform_color_clip_plane_1.rgb : uniform_color_clip_plane_2.rgb;
		color.a = uniform_color.a;
    }
    else
	    color = uniform_color;

    // Orca Zoukei: multi-colour filament (approximation, see zk_multicolor)
    if (mc_mode > 0 && !use_color_clip_plane && !is_outline)
        color.rgb = zk_multicolor(mc_normal, world_pos.xyz);

    if (slope.actived) {
         if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
                // Orca Zoukei: only the walls' 0.1 mm contact rim turns light blue. The face lying on the bed
                // (normal pointing down) keeps its filament colour and painting, so it stays readable from below.
                if (world_normal_z > -0.5) {
                    color.rgb = LightBlue;
                    color.a = 0.8;
                }
         }
         else if( world_normal_z < slope.normal_z - EPSILON)
         {
                color.rgb = color.rgb * 0.5 + LightRed * 0.5;
                color.a = 0.8;
         }
    }
    // if the fragment is outside the print volume -> use darker color
	vec3 pv_check_min = ZERO;
	vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {
		// rectangle
		pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
		pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
	}
	else if (print_volume.type == 1) {
		// circle
		float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
		pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
		pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
	}
	color.rgb = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color.rgb, ZERO, 0.3333) : color.rgb;

    // BBS per-extruder printable-height shading (H2D/H2C). Gated on the flag so it is inert for
    // single-extruder printers. Darkens the band between the two extruders' Z limits inside the bed
    // rect (the zone only the taller extruder can reach). Math kept byte-identical to BBS gouraud.fs.
    if (extruder_printable_heights.x >= 1.0) {
        vec3 eph_check_min = (world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, extruder_printable_heights.y)) * ONE_OVER_EPSILON;
        vec3 eph_check_max = (world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, extruder_printable_heights.z)) * ONE_OVER_EPSILON;
        bool is_out_printable_height = (all(greaterThan(eph_check_min, vec3(1.0))) && all(lessThan(eph_check_max, vec3(1.0))));
        color.rgb = is_out_printable_height ? mix(color.rgb, ZERO, 0.7) : color.rgb;
    }
    float shade = shadow_shade();

    //BBS: add outline_color
    if (is_outline) {
        color = vec4((realistic_view > 0.5 ? realistic_lit(color.rgb, eye_normal, top_light_dir(), world_pos.z, shade) : (vec3(intensity.y) + color.rgb * intensity.x) * shade), color.a);
        vec2 fragCoord = gl_FragCoord.xy;
        float s = DetectSilho(fragCoord);
        // Makes silhouettes thicker.
        for(int i=1;i<=INFLATE; i++)
        {
           s = max(s, DetectSilho(fragCoord.xy + vec2(i, 0)));
           s = max(s, DetectSilho(fragCoord.xy + vec2(0, i)));
        }
        if (s < 0.01)
            discard;
        gl_FragColor = vec4(mix(color.rgb, getBackfaceColor(color.rgb), s), color.a);
    }
#ifdef ENABLE_ENVIRONMENT_MAP
    else if (use_environment_tex)
        gl_FragColor = vec4((0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + 0.8 * color.rgb * intensity.x) * shade, color.a);
#endif
    else
        gl_FragColor = vec4((realistic_view > 0.5 ? realistic_lit(color.rgb, eye_normal, top_light_dir(), world_pos.z, shade) : (vec3(intensity.y) + color.rgb * intensity.x) * shade), color.a);
}