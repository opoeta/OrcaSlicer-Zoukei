// Real serial port for the BMCU flasher (Win32 / POSIX) and the list of candidate ports.
// This is the only BMCU code that touches hardware; the protocol (BmcuFlasher.hpp) only sees the Transport.
// Port settings follow pyserial as BMCU-Flasher uses it: 8N1, no flow control, DTR and RTS asserted on open,
// non-blocking reads, 1 s write timeout.
#ifndef slic3r_Utils_BmcuSerialPort_hpp_
#define slic3r_Utils_BmcuSerialPort_hpp_

#include "BmcuFlasher.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace Bmcu {

struct PortInfo
{
    std::string port;         // "COM5", "/dev/ttyUSB0"
    std::string description;  // friendly name from the OS
    unsigned    vid = 0, pid = 0;
    std::string adapter;      // known adapter name ("CH340/CH341 (WCH)"), empty if unknown
    bool        is_ch340 = false; // 1A86:7523, the converter on the BMCU board (USB mode)
};

// Ports present now; CH340 ports first.
std::vector<PortInfo> list_ports();

// Opens `port` at `baud`. Throws std::runtime_error with the OS message.
std::unique_ptr<Transport> open_serial(const std::string &port, unsigned baud);

}} // namespace Slic3r::Bmcu

#endif
