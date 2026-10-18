///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "SegmentTemplate.hpp"
#include "OpenGLUtils.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <algorithm>
#include <vector>

namespace libvgcode {

//|     /1-------6\     |
//|    / |       | \    |
//|   2--0-------5--7   |
//|    \ |       | /    |
//|      3-------4      | 
// The eight corners the vertex shader knows how to place. Each is sent once and
// referenced by INDEX_DATA below, so the post-transform cache can reuse it across
// the triangles that share it: the shader runs 8 times per segment instead of 24.
static constexpr const std::array<uint8_t, 8> VERTEX_DATA = { 0, 1, 2, 3, 4, 5, 6, 7 };

// 16-bit indices: GCN 1/2 GPUs have no 8-bit index support, and some of their drivers
// emulate it wrongly in instanced draws.
static constexpr const std::array<uint16_t, 24> INDEX_DATA = {
    0, 1, 2, // front spike
    0, 2, 3, // front spike
    0, 3, 4, // right/bottom body 
    0, 4, 5, // right/bottom body 
    0, 5, 6, // left/top body 
    0, 6, 1, // left/top body 
    5, 4, 7, // back spike
    5, 7, 6, // back spike
};

void SegmentTemplate::init()
{
    if (m_vao_id != 0)
        return;

    m_size_in_bytes_gpu += VERTEX_DATA.size() * sizeof(uint8_t) + INDEX_DATA.size() * sizeof(uint16_t);

    int curr_vertex_array;
    glsafe(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curr_vertex_array));
    int curr_array_buffer;
    glsafe(glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &curr_array_buffer));

    glsafe(glGenVertexArrays(1, &m_vao_id));
    glsafe(glBindVertexArray(m_vao_id));

    glsafe(glGenBuffers(1, &m_vbo_id));
    glsafe(glBindBuffer(GL_ARRAY_BUFFER, m_vbo_id));
    glsafe(glBufferData(GL_ARRAY_BUFFER, VERTEX_DATA.size() * sizeof(uint8_t), VERTEX_DATA.data(), GL_STATIC_DRAW));
    glsafe(glEnableVertexAttribArray(0));
#ifdef ENABLE_OPENGL_ES
    glsafe(glVertexAttribPointer(0, 1, GL_UNSIGNED_BYTE, GL_FALSE, 0, (const void*)0));
#else
    glsafe(glVertexAttribIPointer(0, 1, GL_UNSIGNED_BYTE, 0, (const void*)0));
#endif // ENABLE_OPENGL_ES

    // The element buffer binding is part of the vao state, so it is left bound here
    // and restored together with the vao.
    glsafe(glGenBuffers(1, &m_ibo_id));
    glsafe(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo_id));
    glsafe(glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_DATA.size() * sizeof(uint16_t), INDEX_DATA.data(), GL_STATIC_DRAW));

    // ORCA: PrusaSlicer 3.0 preview renderer - one batch of segments, no vertex attributes: the shader
    // pulls everything from gl_VertexID, which an indexed draw sets to the index (8 * s + corner).
    static_assert(8 * PULLING_BATCH - 1 < 0xFFFF, "the batch indices must fit 16 bits (0xFFFF kept free for primitive restart)");
    std::vector<uint16_t> batch_indices;
    batch_indices.reserve(PULLING_BATCH * INDEX_DATA.size());
    for (size_t s = 0; s < PULLING_BATCH; ++s) {
        for (const uint16_t corner : INDEX_DATA) {
            batch_indices.push_back(static_cast<uint16_t>(8 * s + corner));
        }
    }
    glsafe(glGenVertexArrays(1, &m_pulling_vao_id));
    glsafe(glBindVertexArray(m_pulling_vao_id));
    glsafe(glGenBuffers(1, &m_pulling_ibo_id));
    glsafe(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_pulling_ibo_id));
    glsafe(glBufferData(GL_ELEMENT_ARRAY_BUFFER, batch_indices.size() * sizeof(uint16_t), batch_indices.data(), GL_STATIC_DRAW));
    m_size_in_bytes_gpu += batch_indices.size() * sizeof(uint16_t);

    glsafe(glBindBuffer(GL_ARRAY_BUFFER, curr_array_buffer));
    glsafe(glBindVertexArray(curr_vertex_array));
}

void SegmentTemplate::shutdown()
{
    if (m_pulling_ibo_id != 0) {
        glsafe(glDeleteBuffers(1, &m_pulling_ibo_id));
        m_pulling_ibo_id = 0;
    }
    if (m_pulling_vao_id != 0) {
        glsafe(glDeleteVertexArrays(1, &m_pulling_vao_id));
        m_pulling_vao_id = 0;
    }
    if (m_ibo_id != 0) {
        glsafe(glDeleteBuffers(1, &m_ibo_id));
        m_ibo_id = 0;
    }
    if (m_vbo_id != 0) {
        glsafe(glDeleteBuffers(1, &m_vbo_id));
        m_vbo_id = 0;
    }
    if (m_vao_id != 0) {
        glsafe(glDeleteVertexArrays(1, &m_vao_id));
        m_vao_id = 0;
    }

    m_size_in_bytes_gpu = 0;
}

void SegmentTemplate::render(size_t count)
{
    if (m_vao_id == 0 || m_vbo_id == 0 || m_ibo_id == 0 || count == 0)
        return;

    int curr_vertex_array;
    glsafe(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curr_vertex_array));

    glsafe(glBindVertexArray(m_vao_id));
    glsafe(glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(INDEX_DATA.size()), GL_UNSIGNED_SHORT,
                                   nullptr, static_cast<GLsizei>(count)));
    glsafe(glBindVertexArray(curr_vertex_array));
}

void SegmentTemplate::render_pulled(size_t count, int segment_offset_location)
{
    if (m_pulling_vao_id == 0 || m_pulling_ibo_id == 0 || count == 0)
        return;

    int curr_vertex_array;
    glsafe(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curr_vertex_array));

    glsafe(glBindVertexArray(m_pulling_vao_id));
    for (size_t first = 0; first < count; first += PULLING_BATCH) {
        const size_t batch = std::min(PULLING_BATCH, count - first);
        glsafe(glUniform1i(segment_offset_location, static_cast<int>(first)));
        glsafe(glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch * INDEX_DATA.size()), GL_UNSIGNED_SHORT, nullptr));
    }
    glsafe(glBindVertexArray(curr_vertex_array));
}

} // namespace libvgcode
