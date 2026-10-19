// Orca Zoukei: Photo Studio media writers, see StudioMedia.hpp.
#include "StudioMedia.hpp"

#include <boost/nowide/cstdio.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <boost/nowide/convert.hpp>
#endif

namespace Slic3r {
namespace GUI {
namespace Zoukei {

// =====================================================================================================
// GIF
// =====================================================================================================

namespace {

inline int cell_of(int r, int g, int b) { return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3); }

// 4x4 Bayer matrix, the ordered dither threshold
constexpr int kBayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

struct BitWriter
{
    std::vector<uint8_t>& out;
    uint32_t              acc{ 0 };
    int                   bits{ 0 };
    void put(int code, int size)
    {
        acc |= uint32_t(code) << bits;
        bits += size;
        while (bits >= 8) {
            out.push_back(uint8_t(acc & 0xFF));
            acc >>= 8;
            bits -= 8;
        }
    }
    void flush()
    {
        if (bits > 0)
            out.push_back(uint8_t(acc & 0xFF));
        acc  = 0;
        bits = 0;
    }
};

// Variable length LZW of GIF, 8 bit symbols: codes start at 9 bits, grow to 12, the table restarts when full.
void lzw_encode(const std::vector<uint8_t>& idx, std::vector<uint8_t>& out)
{
    const int clear = 256;
    int       code_size = 9;
    int       max_code  = clear + 1;
    // child code of (code, symbol); 0 = none (real codes start at 258)
    std::vector<uint16_t> tree(4096 * 256, 0);
    BitWriter             bw{ out };
    bw.put(clear, code_size);
    int cur = -1;
    for (uint8_t v : idx) {
        if (cur < 0) {
            cur = v;
            continue;
        }
        uint16_t& next = tree[size_t(cur) * 256 + v];
        if (next != 0) {
            cur = next;
            continue;
        }
        bw.put(cur, code_size);
        next = uint16_t(++max_code);
        if (max_code >= (1 << code_size))
            ++code_size;
        if (max_code == 4095) {
            bw.put(clear, code_size);
            std::fill(tree.begin(), tree.end(), uint16_t(0));
            code_size = 9;
            max_code  = clear + 1;
        }
        cur = v;
    }
    if (cur >= 0)
        bw.put(cur, code_size);
    bw.put(clear + 1, code_size);
    bw.flush();
}

} // namespace

GifWriter::~GifWriter()
{
    if (m_file != nullptr)
        std::fclose(m_file);
}

void GifWriter::build_palette(const std::vector<const std::vector<uint8_t>*>& samples, bool transparent)
{
    m_transparent = transparent;
    // histogram of the 5:5:5 cells
    std::vector<uint32_t> hist(32768, 0);
    for (const std::vector<uint8_t>* frame : samples) {
        if (frame == nullptr)
            continue;
        for (size_t i = 0; i + 3 < frame->size(); i += 4) {
            const uint8_t* p = &(*frame)[i];
            if (transparent && p[3] < 128)
                continue;
            ++hist[cell_of(p[0], p[1], p[2])];
        }
    }
    struct Entry { int cell; uint32_t count; };
    std::vector<Entry> entries;
    for (int c = 0; c < 32768; ++c)
        if (hist[c] > 0)
            entries.push_back({ c, hist[c] });
    if (entries.empty())
        entries.push_back({ 0, 1 });

    auto comp = [](int cell, int axis) { return (cell >> (10 - 5 * axis)) & 31; };
    struct Box { size_t begin, end; };
    std::vector<Box> boxes{ { 0, entries.size() } };
    const size_t max_colors = transparent ? 255 : 256;
    while (boxes.size() < max_colors) {
        // split the box with the most pixels times its widest range
        int    best = -1, best_axis = 0;
        double best_score = 0.0;
        for (size_t b = 0; b < boxes.size(); ++b) {
            if (boxes[b].end - boxes[b].begin < 2)
                continue;
            int      lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
            uint64_t count = 0;
            for (size_t i = boxes[b].begin; i < boxes[b].end; ++i) {
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], comp(entries[i].cell, a));
                    hi[a] = std::max(hi[a], comp(entries[i].cell, a));
                }
                count += entries[i].count;
            }
            int axis = 0;
            for (int a = 1; a < 3; ++a)
                if (hi[a] - lo[a] > hi[axis] - lo[axis])
                    axis = a;
            const double score = double(count) * double(hi[axis] - lo[axis] + 1);
            if (hi[axis] > lo[axis] && score > best_score) {
                best_score = score;
                best       = int(b);
                best_axis  = axis;
            }
        }
        if (best < 0)
            break;
        Box box = boxes[best];
        std::sort(entries.begin() + box.begin, entries.begin() + box.end,
                  [&](const Entry& l, const Entry& r) { return comp(l.cell, best_axis) < comp(r.cell, best_axis); });
        uint64_t total = 0;
        for (size_t i = box.begin; i < box.end; ++i)
            total += entries[i].count;
        uint64_t acc   = 0;
        size_t   split = box.begin + 1;
        for (size_t i = box.begin; i < box.end - 1; ++i) {
            acc += entries[i].count;
            split = i + 1;
            if (acc * 2 >= total)
                break;
        }
        boxes[best] = { box.begin, split };
        boxes.push_back({ split, box.end });
    }

    std::fill(m_palette.begin(), m_palette.end(), uint8_t(0));
    const int first = transparent ? 1 : 0;
    int       n     = 0;
    for (const Box& b : boxes) {
        double   sum[3] = { 0.0, 0.0, 0.0 };
        uint64_t count  = 0;
        for (size_t i = b.begin; i < b.end; ++i) {
            for (int a = 0; a < 3; ++a)
                sum[a] += double(comp(entries[i].cell, a) * 8 + 4) * double(entries[i].count);
            count += entries[i].count;
        }
        for (int a = 0; a < 3; ++a)
            m_palette[(first + n) * 3 + a] = uint8_t(std::clamp(sum[a] / double(std::max<uint64_t>(1, count)) + 0.5, 0.0, 255.0));
        ++n;
    }
    const int used = first + n;

    // nearest entry of every cell
    m_lut.assign(32768, uint8_t(first));
    for (int c = 0; c < 32768; ++c) {
        const int r = comp(c, 0) * 8 + 4, g = comp(c, 1) * 8 + 4, bl = comp(c, 2) * 8 + 4;
        int best = first, best_d = std::numeric_limits<int>::max();
        for (int i = first; i < used; ++i) {
            const int dr = r - m_palette[i * 3], dg = g - m_palette[i * 3 + 1], db = bl - m_palette[i * 3 + 2];
            const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
            if (d < best_d) {
                best_d = d;
                best   = i;
            }
        }
        m_lut[c] = uint8_t(best);
    }
}

void GifWriter::write_u16(int v)
{
    std::fputc(v & 0xFF, m_file);
    std::fputc((v >> 8) & 0xFF, m_file);
}

bool GifWriter::open(const std::string& path_utf8, int width, int height)
{
    if (m_lut.empty())
        return false;
    m_file = boost::nowide::fopen(path_utf8.c_str(), "wb");
    if (m_file == nullptr)
        return false;
    m_w = width;
    m_h = height;
    std::fwrite("GIF89a", 1, 6, m_file);
    write_u16(m_w);
    write_u16(m_h);
    std::fputc(0xF7, m_file); // global colour table, 8 bit colour resolution, 256 entries
    std::fputc(0, m_file);    // background index
    std::fputc(0, m_file);    // square pixels
    std::fwrite(m_palette.data(), 1, m_palette.size(), m_file);
    // NETSCAPE2.0: loop forever
    const uint8_t loop[] = { 0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00 };
    std::fwrite(loop, 1, sizeof(loop), m_file);
    return std::ferror(m_file) == 0;
}

bool GifWriter::add_frame(const std::vector<uint8_t>& rgba, int delay_cs)
{
    if (m_file == nullptr || rgba.size() < size_t(m_w) * size_t(m_h) * 4)
        return false;
    std::vector<uint8_t> idx(size_t(m_w) * size_t(m_h));
    for (int y = 0; y < m_h; ++y) {
        for (int x = 0; x < m_w; ++x) {
            const size_t   i = size_t(y) * size_t(m_w) + size_t(x);
            const uint8_t* p = &rgba[i * 4];
            if (m_transparent && p[3] < 128) {
                idx[i] = 0;
                continue;
            }
            // ordered dither of about one 5 bit step
            const int t = (kBayer[(y & 3) * 4 + (x & 3)] * 2 - 15) / 4;
            const int r = std::clamp(int(p[0]) + t, 0, 255);
            const int g = std::clamp(int(p[1]) + t, 0, 255);
            const int b = std::clamp(int(p[2]) + t, 0, 255);
            idx[i] = m_lut[cell_of(r, g, b)];
        }
    }
    // graphic control: replace the previous frame when transparent, delay, transparent index
    const uint8_t gce[] = { 0x21, 0xF9, 0x04, uint8_t(m_transparent ? 0x09 : 0x04), uint8_t(delay_cs & 0xFF), uint8_t((delay_cs >> 8) & 0xFF), 0x00, 0x00 };
    std::fwrite(gce, 1, sizeof(gce), m_file);
    std::fputc(0x2C, m_file);
    write_u16(0);
    write_u16(0);
    write_u16(m_w);
    write_u16(m_h);
    std::fputc(0, m_file);
    std::fputc(8, m_file); // LZW minimum code size
    std::vector<uint8_t> data;
    data.reserve(idx.size() / 2);
    lzw_encode(idx, data);
    for (size_t i = 0; i < data.size(); i += 255) {
        const size_t n = std::min<size_t>(255, data.size() - i);
        std::fputc(int(n), m_file);
        std::fwrite(&data[i], 1, n, m_file);
    }
    std::fputc(0, m_file);
    return std::ferror(m_file) == 0;
}

bool GifWriter::close()
{
    if (m_file == nullptr)
        return false;
    std::fputc(0x3B, m_file);
    const bool ok     = std::ferror(m_file) == 0;
    const bool closed = std::fclose(m_file) == 0;
    m_file = nullptr;
    return ok && closed;
}

// =====================================================================================================
// MP4 (Windows Media Foundation)
// =====================================================================================================

#ifdef _WIN32

namespace {
template<class T> void release(T*& p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}
std::string hr_text(const char* what, HRESULT hr)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s (0x%08lX)", what, (unsigned long) hr);
    return buf;
}
} // namespace

struct Mp4Writer::Impl
{
    IMFSinkWriter*       writer{ nullptr };
    DWORD                stream{ 0 };
    LONGLONG             time{ 0 };
    LONGLONG             duration{ 0 };
    int                  w{ 0 }, h{ 0 };
    bool                 started{ false };
    std::vector<uint8_t> nv12;
};

Mp4Writer::Mp4Writer() : m_impl(new Impl) {}

Mp4Writer::~Mp4Writer()
{
    std::string ignored;
    if (m_impl->writer != nullptr)
        close(ignored);
}

bool Mp4Writer::available() { return true; }

bool Mp4Writer::open(const std::string& path_utf8, int width, int height, int fps, std::string& error)
{
    Impl& d = *m_impl;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) || fps <= 0) {
        error = "Invalid video size.";
        return false;
    }
    HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        error = hr_text("Media Foundation is not available", hr);
        return false;
    }
    d.started  = true;
    d.w        = width;
    d.h        = height;
    d.time     = 0;
    d.duration = 10000000LL / fps;

    IMFAttributes* attrs = nullptr;
    IMFMediaType*  out   = nullptr;
    IMFMediaType*  in    = nullptr;
    hr = ::MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(hr))
        hr = attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr))
        hr = attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    const std::wstring wpath = boost::nowide::widen(path_utf8);
    if (SUCCEEDED(hr))
        hr = ::MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attrs, &d.writer);
    if (FAILED(hr))
        error = hr_text("The video file could not be created", hr);

    // H.264 High profile out, progressive, square pixels, BT.709
    const UINT32 bitrate = (UINT32) std::clamp<double>(double(width) * double(height) * double(fps) * 0.16, 1.0e6, 6.0e7);
    if (SUCCEEDED(hr))
        hr = ::MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    if (SUCCEEDED(hr)) hr = ::MFSetAttributeSize(out, MF_MT_FRAME_SIZE, (UINT32) width, (UINT32) height);
    if (SUCCEEDED(hr)) hr = ::MFSetAttributeRatio(out, MF_MT_FRAME_RATE, (UINT32) fps, 1);
    if (SUCCEEDED(hr)) hr = ::MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) {
        out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        hr = d.writer->AddStream(out, &d.stream);
    }
    // NV12 in: converted here, so no colour converter has to be found on the system
    if (SUCCEEDED(hr)) hr = ::MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32) width);
    if (SUCCEEDED(hr)) hr = ::MFSetAttributeSize(in, MF_MT_FRAME_SIZE, (UINT32) width, (UINT32) height);
    if (SUCCEEDED(hr)) hr = ::MFSetAttributeRatio(in, MF_MT_FRAME_RATE, (UINT32) fps, 1);
    if (SUCCEEDED(hr)) hr = ::MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) {
        in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        in->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        hr = d.writer->SetInputMediaType(d.stream, in, nullptr);
        if (FAILED(hr) && error.empty())
            error = hr_text("No H.264 encoder accepted this video size", hr);
    }
    if (SUCCEEDED(hr))
        hr = d.writer->BeginWriting();
    if (FAILED(hr) && error.empty())
        error = hr_text("The H.264 encoder could not start", hr);

    release(attrs);
    release(out);
    release(in);
    if (FAILED(hr)) {
        release(d.writer);
        ::MFShutdown();
        d.started = false;
        return false;
    }
    d.nv12.assign(size_t(width) * size_t(height) * 3 / 2, 0);
    return true;
}

bool Mp4Writer::add_frame(const std::vector<uint8_t>& rgba, std::string& error)
{
    Impl& d = *m_impl;
    if (d.writer == nullptr || rgba.size() < size_t(d.w) * size_t(d.h) * 4)
        return false;
    // RGB -> NV12, BT.709 limited range
    const int w = d.w, h = d.h;
    uint8_t*  Y  = d.nv12.data();
    uint8_t*  UV = d.nv12.data() + size_t(w) * size_t(h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = &rgba[size_t(y) * size_t(w) * 4];
        uint8_t*       dst = Y + size_t(y) * size_t(w);
        for (int x = 0; x < w; ++x, src += 4)
            dst[x] = uint8_t(std::clamp(16.0f + (46.559f * src[0] + 156.629f * src[1] + 15.812f * src[2]) / 255.0f + 0.5f, 0.0f, 255.0f));
    }
    for (int y = 0; y < h; y += 2) {
        uint8_t* dst = UV + size_t(y / 2) * size_t(w);
        for (int x = 0; x < w; x += 2) {
            float r = 0.f, g = 0.f, b = 0.f;
            for (int k = 0; k < 4; ++k) {
                const uint8_t* s = &rgba[(size_t(y + (k >> 1)) * size_t(w) + size_t(x + (k & 1))) * 4];
                r += s[0];
                g += s[1];
                b += s[2];
            }
            r *= 0.25f;
            g *= 0.25f;
            b *= 0.25f;
            dst[x]     = uint8_t(std::clamp(128.0f + (-25.664f * r - 86.336f * g + 112.0f * b) / 255.0f + 0.5f, 0.0f, 255.0f));
            dst[x + 1] = uint8_t(std::clamp(128.0f + (112.0f * r - 101.730f * g - 10.270f * b) / 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }

    IMFMediaBuffer* buffer = nullptr;
    IMFSample*      sample = nullptr;
    const DWORD     size   = (DWORD) d.nv12.size();
    HRESULT         hr     = ::MFCreateMemoryBuffer(size, &buffer);
    BYTE*           data   = nullptr;
    if (SUCCEEDED(hr))
        hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        std::memcpy(data, d.nv12.data(), size);
        buffer->Unlock();
        hr = buffer->SetCurrentLength(size);
    }
    if (SUCCEEDED(hr)) hr = ::MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(d.time);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(d.duration);
    if (SUCCEEDED(hr)) hr = d.writer->WriteSample(d.stream, sample);
    release(sample);
    release(buffer);
    if (FAILED(hr)) {
        error = hr_text("A video frame could not be encoded", hr);
        return false;
    }
    d.time += d.duration;
    return true;
}

bool Mp4Writer::close(std::string& error)
{
    Impl& d  = *m_impl;
    bool  ok = true;
    if (d.writer != nullptr) {
        const HRESULT hr = d.writer->Finalize();
        if (FAILED(hr)) {
            error = hr_text("The video file could not be finished", hr);
            ok    = false;
        }
        release(d.writer);
    }
    if (d.started) {
        ::MFShutdown();
        d.started = false;
    }
    return ok;
}

#else // _WIN32

struct Mp4Writer::Impl
{};

Mp4Writer::Mp4Writer() : m_impl(new Impl) {}
Mp4Writer::~Mp4Writer() = default;
bool Mp4Writer::available() { return false; }
bool Mp4Writer::open(const std::string&, int, int, int, std::string& error)
{
    error = "MP4 export needs Windows Media Foundation; use GIF or a PNG sequence on this system.";
    return false;
}
bool Mp4Writer::add_frame(const std::vector<uint8_t>&, std::string&) { return false; }
bool Mp4Writer::close(std::string&) { return false; }

#endif // _WIN32

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r
