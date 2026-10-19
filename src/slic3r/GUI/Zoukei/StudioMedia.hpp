// Orca Zoukei: Photo Studio media writers.
//  - GifWriter: animated GIF89a written here (no third-party code): a median cut palette shared by all frames,
//    ordered (Bayer) dithering, which stays still from frame to frame unlike error diffusion, LZW, looping.
//  - Mp4Writer: H.264 MP4 through the Windows Media Foundation sink writer (mfplat / mfreadwrite, part of
//    Windows, no new dependency). Not available on macOS / Linux: the studio offers GIF or a PNG sequence there.
// Frames are RGBA, top row first, straight alpha.
#ifndef slic3r_GUI_Zoukei_StudioMedia_hpp_
#define slic3r_GUI_Zoukei_StudioMedia_hpp_

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace Zoukei {

class GifWriter
{
public:
    GifWriter() = default;
    ~GifWriter();
    GifWriter(const GifWriter&) = delete;
    GifWriter& operator=(const GifWriter&) = delete;

    // The palette, from a few frames that show what the animation will (all of them is best, a handful is enough).
    // transparent: pixels with alpha < 128 become the transparent index 0.
    void build_palette(const std::vector<const std::vector<uint8_t>*>& samples, bool transparent);
    bool open(const std::string& path_utf8, int width, int height);
    // delay in hundredths of a second
    bool add_frame(const std::vector<uint8_t>& rgba, int delay_cs);
    bool close();

private:
    void write_u16(int v);

    std::FILE*           m_file{ nullptr };
    int                  m_w{ 0 }, m_h{ 0 };
    bool                 m_transparent{ false };
    std::vector<uint8_t> m_palette = std::vector<uint8_t>(256 * 3, 0);
    // nearest palette entry of each 5:5:5 colour cell
    std::vector<uint8_t> m_lut;
};

class Mp4Writer
{
public:
    Mp4Writer();
    ~Mp4Writer();
    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    // False on macOS / Linux.
    static bool available();
    // width and height even.
    bool open(const std::string& path_utf8, int width, int height, int fps, std::string& error);
    // Alpha is ignored: composite the frame first.
    bool add_frame(const std::vector<uint8_t>& rgba, std::string& error);
    bool close(std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Zoukei_StudioMedia_hpp_
