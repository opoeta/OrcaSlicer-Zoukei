// Real serial port for the BMCU flasher. See BmcuSerialPort.hpp.
#include "BmcuSerialPort.hpp"
#include "Serial.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <boost/nowide/convert.hpp>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif
#endif

namespace Slic3r { namespace Bmcu {

std::vector<PortInfo> list_ports()
{
    std::vector<PortInfo> out;
    for (const Utils::SerialPortInfo &sp : Utils::scan_serial_ports_extended()) {
        PortInfo p;
        p.port        = sp.port;
        p.description = sp.friendly_name.empty() ? sp.port : sp.friendly_name;
        if (sp.id_vendor != unsigned(-1) && sp.id_product != unsigned(-1)) {
            p.vid = sp.id_vendor;
            p.pid = sp.id_product;
        }
        for (const Adapter &a : known_adapters())
            if (a.vid == p.vid && a.pid == p.pid)
                p.adapter = a.name;
        p.is_ch340 = p.vid == CH340_VID && p.pid == CH340_PID;
        out.push_back(std::move(p));
    }
    std::stable_sort(out.begin(), out.end(), [](const PortInfo &a, const PortInfo &b) {
        if (a.is_ch340 != b.is_ch340) return a.is_ch340;
        return !a.adapter.empty() && b.adapter.empty();
    });
    return out;
}

#ifdef _WIN32

namespace {
std::string last_error_text(const char *what)
{
    const DWORD err = GetLastError();
    char *buf = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, (LPSTR) &buf, 0, nullptr);
    std::string msg = std::string(what) + " (error " + std::to_string(err) + (buf ? std::string(": ") + buf : std::string()) + ")";
    if (buf)
        LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' '))
        msg.pop_back();
    return msg;
}

class Win32Serial : public Transport
{
public:
    Win32Serial(const std::string &port, unsigned baud)
    {
        const std::wstring path = L"\\\\.\\" + boost::nowide::widen(port);
        m_h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (m_h == INVALID_HANDLE_VALUE)
            throw std::runtime_error(last_error_text(("cannot open " + port).c_str()));
        DCB dcb;
        std::memset(&dcb, 0, sizeof(dcb));
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(m_h, &dcb)) {
            const std::string e = last_error_text("GetCommState");
            CloseHandle(m_h);
            throw std::runtime_error(e);
        }
        dcb.BaudRate     = baud;
        dcb.ByteSize     = 8;
        dcb.Parity       = NOPARITY;
        dcb.StopBits     = ONESTOPBIT;
        dcb.fBinary      = TRUE;
        dcb.fParity      = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl  = DTR_CONTROL_ENABLE;
        dcb.fRtsControl  = RTS_CONTROL_ENABLE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX  = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;
        if (!SetCommState(m_h, &dcb)) {
            const std::string e = last_error_text("SetCommState");
            CloseHandle(m_h);
            throw std::runtime_error(e);
        }
        COMMTIMEOUTS to;
        std::memset(&to, 0, sizeof(to));
        to.ReadIntervalTimeout        = MAXDWORD; // return at once with what is there
        to.WriteTotalTimeoutConstant  = 1000;
        SetCommTimeouts(m_h, &to);
        SetupComm(m_h, 4096, 4096);
        PurgeComm(m_h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }
    ~Win32Serial() override { Win32Serial::close(); }

    void write(const uint8_t *data, size_t n) override
    {
        check_open();
        DWORD written = 0;
        if (!WriteFile(m_h, data, DWORD(n), &written, nullptr) || written != n)
            throw std::runtime_error(last_error_text("serial write failed"));
    }
    size_t read_available(uint8_t *buf, size_t cap) override
    {
        check_open();
        DWORD got = 0;
        if (!ReadFile(m_h, buf, DWORD(cap), &got, nullptr))
            throw std::runtime_error(last_error_text("serial read failed"));
        return size_t(got);
    }
    void purge() override
    {
        check_open();
        PurgeComm(m_h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }
    void set_baud(unsigned baud) override
    {
        check_open();
        DCB dcb;
        std::memset(&dcb, 0, sizeof(dcb));
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(m_h, &dcb))
            throw std::runtime_error(last_error_text("GetCommState"));
        dcb.BaudRate = baud;
        if (!SetCommState(m_h, &dcb))
            throw std::runtime_error(last_error_text("SetCommState (baud)"));
    }
    void set_dtr(bool on) override
    {
        check_open();
        EscapeCommFunction(m_h, on ? SETDTR : CLRDTR);
    }
    void set_rts(bool on) override
    {
        check_open();
        EscapeCommFunction(m_h, on ? SETRTS : CLRRTS);
    }
    void close() override
    {
        if (m_h == INVALID_HANDLE_VALUE)
            return;
        EscapeCommFunction(m_h, SETDTR);
        EscapeCommFunction(m_h, SETRTS);
        CloseHandle(m_h);
        m_h = INVALID_HANDLE_VALUE;
    }

private:
    void check_open() const
    {
        if (m_h == INVALID_HANDLE_VALUE)
            throw std::runtime_error("serial port is closed");
    }
    HANDLE m_h = INVALID_HANDLE_VALUE;
};
} // namespace

std::unique_ptr<Transport> open_serial(const std::string &port, unsigned baud) { return std::make_unique<Win32Serial>(port, baud); }

#else // POSIX

namespace {
std::string errno_text(const std::string &what) { return what + " (" + std::strerror(errno) + ")"; }

bool std_speed(unsigned baud, speed_t &out)
{
    switch (baud) {
    case 9600: out = B9600; return true;
    case 19200: out = B19200; return true;
    case 38400: out = B38400; return true;
    case 57600: out = B57600; return true;
    case 115200: out = B115200; return true;
    case 230400: out = B230400; return true;
#ifdef B460800
    case 460800: out = B460800; return true;
#endif
#ifdef B921600
    case 921600: out = B921600; return true;
#endif
#ifdef B1000000
    case 1000000: out = B1000000; return true;
#endif
    default: return false;
    }
}

class PosixSerial : public Transport
{
public:
    PosixSerial(const std::string &port, unsigned baud)
    {
        m_fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (m_fd < 0)
            throw std::runtime_error(errno_text("cannot open " + port));
        termios tio;
        if (tcgetattr(m_fd, &tio) != 0) {
            const std::string e = errno_text("tcgetattr");
            ::close(m_fd);
            m_fd = -1;
            throw std::runtime_error(e);
        }
        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~(CSTOPB | PARENB | CSIZE);
        tio.c_cflag |= CS8;
#ifdef CRTSCTS
        tio.c_cflag &= ~CRTSCTS;
#endif
        tio.c_iflag &= ~(IXON | IXOFF | IXANY);
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 0;
        if (tcsetattr(m_fd, TCSANOW, &tio) != 0) {
            const std::string e = errno_text("tcsetattr");
            ::close(m_fd);
            m_fd = -1;
            throw std::runtime_error(e);
        }
        set_baud(baud);
        set_dtr(true);
        set_rts(true);
        tcflush(m_fd, TCIOFLUSH);
    }
    ~PosixSerial() override { PosixSerial::close(); }

    void write(const uint8_t *data, size_t n) override
    {
        check_open();
        const uint64_t end = now_ms() + 1000;
        size_t done = 0;
        while (done < n) {
            const ssize_t w = ::write(m_fd, data + done, n - done);
            if (w > 0) {
                done += size_t(w);
                continue;
            }
            if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                throw std::runtime_error(errno_text("serial write failed"));
            if (now_ms() >= end)
                throw std::runtime_error("serial write timeout");
            pollfd p{m_fd, POLLOUT, 0};
            ::poll(&p, 1, 10);
        }
        tcdrain(m_fd);
    }
    size_t read_available(uint8_t *buf, size_t cap) override
    {
        check_open();
        const ssize_t r = ::read(m_fd, buf, cap);
        if (r > 0)
            return size_t(r);
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            throw std::runtime_error(errno_text("serial read failed"));
        return 0;
    }
    void purge() override
    {
        check_open();
        tcflush(m_fd, TCIOFLUSH);
    }
    void set_baud(unsigned baud) override
    {
        check_open();
        speed_t sp;
#ifdef __APPLE__
        termios tio;
        tcgetattr(m_fd, &tio);
        if (std_speed(baud, sp)) {
            cfsetispeed(&tio, sp);
            cfsetospeed(&tio, sp);
            if (tcsetattr(m_fd, TCSANOW, &tio) != 0)
                throw std::runtime_error(errno_text("tcsetattr (baud)"));
        } else {
            speed_t custom = speed_t(baud);
            if (ioctl(m_fd, IOSSIOSPEED, &custom) != 0)
                throw std::runtime_error(errno_text("IOSSIOSPEED"));
        }
#else
        if (!std_speed(baud, sp))
            throw std::runtime_error("baud rate " + std::to_string(baud) + " not supported on this system");
        termios tio;
        tcgetattr(m_fd, &tio);
        cfsetispeed(&tio, sp);
        cfsetospeed(&tio, sp);
        if (tcsetattr(m_fd, TCSANOW, &tio) != 0)
            throw std::runtime_error(errno_text("tcsetattr (baud)"));
#endif
    }
    void set_dtr(bool on) override { modem_bit(TIOCM_DTR, on); }
    void set_rts(bool on) override { modem_bit(TIOCM_RTS, on); }
    void close() override
    {
        if (m_fd < 0)
            return;
        int bits = TIOCM_DTR | TIOCM_RTS;
        ioctl(m_fd, TIOCMBIS, &bits);
        ::close(m_fd);
        m_fd = -1;
    }

private:
    void check_open() const
    {
        if (m_fd < 0)
            throw std::runtime_error("serial port is closed");
    }
    void modem_bit(int bit, bool on)
    {
        check_open();
        if (ioctl(m_fd, on ? TIOCMBIS : TIOCMBIC, &bit) != 0)
            throw std::runtime_error(errno_text("modem line ioctl"));
    }
    int m_fd = -1;
};
} // namespace

std::unique_ptr<Transport> open_serial(const std::string &port, unsigned baud) { return std::make_unique<PosixSerial>(port, baud); }

#endif

}} // namespace Slic3r::Bmcu
