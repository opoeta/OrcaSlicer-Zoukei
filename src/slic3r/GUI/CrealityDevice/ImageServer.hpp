// Local image relay for the embedded Creality device panel.
//
// The panel shows the printer's current-print picture and G-code thumbnails through
// http://localhost:<port>/device_image?address=&path=&secure= (Creality Print serves this from
// its HttpServer, CrealityOfficial/CrealityPrint@59ae8cbb0437dcc7236e27433ecd4aeb52be0244,
// AGPL-3.0). This small server answers only that route, on 127.0.0.1, and fetches only the
// image paths the panel uses, from LAN printers accepted by the host filter.
#ifndef slic3r_CrealityDevice_ImageServer_hpp_
#define slic3r_CrealityDevice_ImageServer_hpp_

#include <functional>
#include <memory>
#include <string>

namespace Slic3r { namespace GUI { namespace CrealityDevice {

class ImageServer
{
public:
    // Called on the server's threads; must be thread safe.
    using HostFilter = std::function<bool(const std::string& host)>;

    explicit ImageServer(HostFilter filter);
    ~ImageServer();

    ImageServer(const ImageServer&)            = delete;
    ImageServer& operator=(const ImageServer&) = delete;

    // Listening port on 127.0.0.1, 0 when the server could not start.
    unsigned short port() const;
    void           stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}}} // namespace Slic3r::GUI::CrealityDevice

#endif // slic3r_CrealityDevice_ImageServer_hpp_
