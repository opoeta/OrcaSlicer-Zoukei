#version 140

uniform bool ban_light;
uniform vec4 uniform_color;
uniform float emission_factor;

// x = tainted, y = specular;
in vec2 intensity;
//varying float drop;
in vec4 world_pos;

out vec4 out_color;

void main()
{
    // Orca Zoukei: same tolerance as SINKING_Z_THRESHOLD (Model.hpp). A face a few microns under the bed (an SVG inlay
    // whose bottom was rounded to 4 decimals) still rests on it and must not vanish from the thumbnail.
    if (world_pos.z < -0.001)
        discard;
    if(ban_light){
       out_color = uniform_color;
    } else{
       out_color = vec4(vec3(intensity.y) + uniform_color.rgb * (intensity.x + emission_factor), uniform_color.a);
    }
}
