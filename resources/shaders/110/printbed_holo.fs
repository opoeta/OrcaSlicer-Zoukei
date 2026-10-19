#version 110

// Orca Zoukei: iridescent holographic plate (see the 140 version for the notes).
uniform sampler2D texture;
uniform vec3 camera_pos;
uniform float effect_strength; // 0..1

varying vec2 tex_coord;
varying vec3 world_pos;

vec3 hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main()
{
    vec4 tex = texture2D(texture, tex_coord);
    vec3 v = normalize(camera_pos - world_pos);
    float lum = dot(tex.rgb, vec3(0.299, 0.587, 0.114));

    float tilt = 1.0 - clamp(v.z, 0.0, 1.0);
    float hue = fract(0.62 + tilt * 1.35 + atan(v.y, v.x) * 0.05 + dot(tex_coord, vec2(0.45, 0.30)));
    vec3 rainbow = hsv2rgb(vec3(hue, 0.55, 1.0));

    float k = effect_strength * (0.30 + 0.70 * smoothstep(0.15, 0.95, lum));
    vec3 col = mix(tex.rgb, tex.rgb * (0.55 + rainbow * 0.95), k * 0.55);
    col += (rainbow - 0.5) * 0.10 * k;

    float spot  = pow(clamp(v.z, 0.0, 1.0), 40.0);
    float sweep = exp(-pow(dot(v.xy, vec2(0.7071, -0.7071)) / 0.10, 2.0)) * (0.35 + 0.65 * tilt);
    col += vec3(1.0) * (spot * 0.10 + sweep * 0.10) * effect_strength;

    gl_FragColor = vec4(clamp(col, 0.0, 1.0), tex.a);
}
