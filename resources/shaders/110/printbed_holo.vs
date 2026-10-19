#version 110

// Orca Zoukei: holographic plate texture, vertex stage (world position goes to the fragment stage)
uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

attribute vec3 v_position;
attribute vec2 v_tex_coord;

varying vec2 tex_coord;
varying vec3 world_pos;

void main()
{
    tex_coord = v_tex_coord;
    world_pos = v_position;
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
