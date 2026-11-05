// Bridges the nRF52840 Meshtastic BLE service to meshtasticd's TCP API.
//
// The nRF52840 AT firmware (Xinyuan-LilyGO/T-Display-K230-nRF52840) serves the
// standard Meshtastic GATT service and passes raw protobufs over /dev/ttyS1:
//   phone writes toRadio  -> "+MESH:TORADIO,<len>,<hex>"
//   "AT+MESHFROM=<hex>"   -> queued for fromRadio reads, fromNum notified
// meshtasticd's TCP API (port 4403) carries the same ToRadio/FromRadio
// protobufs framed as 0x94 0xC3 <len16 BE> <payload>.
//
// State for the launcher (advertised name, phone connection, pairing PIN) goes
// to /tmp/k230_meshtastic_ble_status.
//
// One TCP session is held per BLE connection, so meshtasticd keeps queueing
// for the phone while it's away. The nRF queue holds 8 packets; the bridge
// keeps a local FIFO and sends one AT+MESHFROM at a time.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <deque>
#include <string>
#include <vector>

namespace {

constexpr size_t kMaxPacket = 512;      // nRF MESH_BRIDGE_MAX_PACKET
constexpr int kNrfQueueDepth = 8;       // nRF MESH_BRIDGE_QUEUE_DEPTH
constexpr size_t kFifoMax = 4096;
constexpr uint64_t kAckTimeoutMs = 1500;
constexpr uint64_t kHeartbeatMs = 300000;  // meshtasticd drops idle clients at 15 min
constexpr uint64_t kNameRetryMs = 5000;
constexpr uint32_t kConfigOnlyNonce = 69420;  // SPECIAL_NONCE_ONLY_CONFIG

const char *g_uart_path = "/dev/ttyS1";
const char *g_host = "127.0.0.1";
int g_port = 4403;
// Read by the launcher's Meshtastic page: name=, phone=, pin= lines.
const char *g_status_path = "/tmp/k230_meshtastic_ble_status";
std::string g_name_override;
volatile sig_atomic_t g_stop = 0;

uint64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void log_msg(const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    char stamp[32];
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
    fprintf(stderr, "%s.%03ldZ ", stamp, ts.tv_nsec / 1000000L);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

std::string hex_encode(const uint8_t *data, size_t len)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2);
    for(size_t i = 0; i < len; i++) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

int hex_nibble(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_decode(const char *hex, size_t hex_len, std::vector<uint8_t> *out)
{
    if(hex_len % 2U != 0U) {
        return false;
    }
    out->clear();
    out->reserve(hex_len / 2U);
    for(size_t i = 0; i < hex_len; i += 2U) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1U]);
        if(hi < 0 || lo < 0) {
            return false;
        }
        out->push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

// ---- Minimal protobuf reading, enough for my_info and the owner's name ----

bool pb_varint(const uint8_t *p, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t v = 0;
    for(int shift = 0; shift < 64 && *pos < len; shift += 7) {
        uint8_t b = p[(*pos)++];
        v |= (uint64_t)(b & 0x7f) << shift;
        if(!(b & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

// Calls fn(field, wire_type, varint, data, data_len) for each field.
template <typename Fn>
bool pb_walk(const uint8_t *p, size_t len, Fn fn)
{
    size_t pos = 0;
    while(pos < len) {
        uint64_t key = 0;
        uint64_t v = 0;
        if(!pb_varint(p, len, &pos, &key)) return false;
        uint32_t field = (uint32_t)(key >> 3);
        uint32_t wt = (uint32_t)(key & 7);
        switch(wt) {
        case 0:
            if(!pb_varint(p, len, &pos, &v)) return false;
            fn(field, wt, v, nullptr, 0);
            break;
        case 1:
            if(pos + 8 > len) return false;
            pos += 8;
            break;
        case 2:
            if(!pb_varint(p, len, &pos, &v) || v > len - pos) return false;
            fn(field, wt, 0, p + pos, (size_t)v);
            pos += (size_t)v;
            break;
        case 5:
            if(pos + 4 > len) return false;
            pos += 4;
            break;
        default:
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> toradio_want_config(uint32_t nonce)
{
    std::vector<uint8_t> out = { 0x18 };  // field 3, varint
    uint32_t v = nonce;
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        out.push_back(v ? (uint8_t)(b | 0x80) : b);
    } while(v);
    return out;
}

const std::vector<uint8_t> kToRadioHeartbeat = { 0x3a, 0x00 };   // heartbeat {}
const std::vector<uint8_t> kToRadioDisconnect = { 0x20, 0x01 };  // disconnect: true

// ---- TCP stream to meshtasticd ----

class ApiStream {
public:
    ~ApiStream() { close_fd(); }

    bool open_fd()
    {
        struct sockaddr_in addr;
        int one = 1;

        close_fd();
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if(fd_ < 0) {
            return false;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)g_port);
        inet_pton(AF_INET, g_host, &addr.sin_addr);
        if(connect(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            int err = errno;
            close_fd();
            errno = err;
            return false;
        }
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
        rx_.clear();
        last_tx_ms_ = now_ms();
        return true;
    }

    void close_fd()
    {
        if(fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        rx_.clear();
    }

    int fd() const { return fd_; }
    bool is_open() const { return fd_ >= 0; }
    uint64_t last_tx_ms() const { return last_tx_ms_; }

    bool send_frame(const std::vector<uint8_t> &payload)
    {
        uint8_t hdr[4] = { 0x94, 0xc3, (uint8_t)(payload.size() >> 8),
                           (uint8_t)(payload.size() & 0xff) };
        std::vector<uint8_t> buf(hdr, hdr + 4);
        buf.insert(buf.end(), payload.begin(), payload.end());
        size_t off = 0;
        while(off < buf.size()) {
            ssize_t n = send(fd_, buf.data() + off, buf.size() - off, MSG_NOSIGNAL);
            if(n < 0) {
                if(errno == EINTR) continue;
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd = { fd_, POLLOUT, 0 };
                    poll(&pfd, 1, 200);
                    continue;
                }
                return false;
            }
            off += (size_t)n;
        }
        last_tx_ms_ = now_ms();
        return true;
    }

    // Reads what's available and appends whole frames. Returns false on EOF/error.
    bool read_frames(std::deque<std::vector<uint8_t>> *frames)
    {
        uint8_t buf[2048];
        for(;;) {
            ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if(n == 0) return false;
            if(n < 0) {
                if(errno == EINTR) continue;
                if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                return false;
            }
            rx_.insert(rx_.end(), buf, buf + n);
        }
        // Bytes outside a 0x94 0xC3 frame are debug text; skip them.
        size_t pos = 0;
        while(rx_.size() - pos >= 4) {
            if(rx_[pos] != 0x94 || rx_[pos + 1] != 0xc3) {
                pos++;
                continue;
            }
            size_t len = ((size_t)rx_[pos + 2] << 8) | rx_[pos + 3];
            if(len > kMaxPacket) {
                pos++;
                continue;
            }
            if(rx_.size() - pos - 4 < len) break;
            frames->emplace_back(rx_.begin() + (long)pos + 4,
                                 rx_.begin() + (long)(pos + 4 + len));
            pos += 4 + len;
        }
        rx_.erase(rx_.begin(), rx_.begin() + (long)pos);
        return true;
    }

private:
    int fd_ = -1;
    std::vector<uint8_t> rx_;
    uint64_t last_tx_ms_ = 0;
};

// Tracks this node's number, short name and MAC from FromRadio frames, to
// build the BLE name the firmware's getDeviceName() would: "<short_name>_abcd",
// or "Meshtastic_abcd" while the short name is still the default, where abcd
// is the last two MAC bytes.
struct OwnerInfo {
    uint32_t my_node = 0;
    std::string short_name;
    std::string mac;

    // Returns true for config_complete_id == nonce.
    bool feed(const std::vector<uint8_t> &f, uint32_t nonce = 0)
    {
        bool complete = false;
        pb_walk(f.data(), f.size(), [&](uint32_t field, uint32_t, uint64_t v,
                                        const uint8_t *d, size_t dl) {
            if(field == 3 && d) {  // my_info
                pb_walk(d, dl, [&](uint32_t mf, uint32_t, uint64_t mv, const uint8_t *, size_t) {
                    if(mf == 1) my_node = (uint32_t)mv;
                });
            } else if(field == 4 && d) {  // node_info
                uint32_t num = 0;
                std::string sn;
                std::string mc;
                pb_walk(d, dl, [&](uint32_t nf, uint32_t, uint64_t nv,
                                   const uint8_t *nd, size_t ndl) {
                    if(nf == 1) num = (uint32_t)nv;
                    if(nf == 2 && nd) {  // user
                        pb_walk(nd, ndl, [&](uint32_t uf, uint32_t, uint64_t,
                                             const uint8_t *ud, size_t udl) {
                            if(uf == 3 && ud) sn.assign((const char *)ud, udl);
                            if(uf == 4 && ud) mc.assign((const char *)ud, udl);
                        });
                    }
                });
                if(num != 0 && num == my_node) {
                    short_name = sn;
                    if(mc.size() == 6) mac = mc;
                }
            } else if(field == 7 && nonce != 0 && v == nonce) {  // config_complete_id
                complete = true;
            }
        });
        return complete;
    }

    std::string ble_name() const
    {
        if(my_node == 0) return "";
        char tail[8];
        if(mac.size() == 6) {
            snprintf(tail, sizeof(tail), "%02x%02x", (uint8_t)mac[4], (uint8_t)mac[5]);
        } else {
            snprintf(tail, sizeof(tail), "%04x", my_node & 0xffffU);
        }
        std::string base = short_name.empty() || short_name == tail ? "Meshtastic" : short_name;
        for(char &c : base) {
            if((unsigned char)c < 0x20 || c == ',' || c == '\r' || c == '\n') c = '_';
        }
        std::string suffix = std::string("_") + tail;
        if(base.size() + suffix.size() > 29) base.resize(29 - suffix.size());
        return base + suffix;
    }
};

// One config-only session (no node DB, no satellite replay) to learn the name
// at startup. Later renames are picked up from the phone's own sessions, since
// a session opened while packets wait for the phone would swallow them.
bool lookup_ble_name(std::string *name)
{
    ApiStream api;
    OwnerInfo owner;
    bool done = false;

    if(!api.open_fd() || !api.send_frame(toradio_want_config(kConfigOnlyNonce))) {
        return false;
    }
    uint64_t deadline = now_ms() + 15000;
    while(!done && now_ms() < deadline) {
        struct pollfd pfd = { api.fd(), POLLIN, 0 };
        if(poll(&pfd, 1, 500) <= 0) continue;
        std::deque<std::vector<uint8_t>> frames;
        if(!api.read_frames(&frames)) break;
        for(const auto &f : frames) {
            done = owner.feed(f, kConfigOnlyNonce) || done;
        }
    }
    (void)api.send_frame(kToRadioDisconnect);
    *name = owner.ble_name();
    return !name->empty();
}

// ---- UART to the nRF52840 ----

int uart_open(const char *path)
{
    struct termios tio;
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) return -1;
    if(tcgetattr(fd, &tio) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CRTSCTS | CSTOPB);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    if(tcsetattr(fd, TCSANOW, &tio) != 0) {
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

bool uart_send_line(int fd, const std::string &line)
{
    std::string buf = line + "\r\n";
    size_t off = 0;
    while(off < buf.size()) {
        ssize_t n = write(fd, buf.data() + off, buf.size() - off);
        if(n < 0) {
            if(errno == EINTR) continue;
            if(errno == EAGAIN) {
                struct pollfd pfd = { fd, POLLOUT, 0 };
                poll(&pfd, 1, 200);
                continue;
            }
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

class Bridge {
public:
    explicit Bridge(int uart) : uart_(uart) {}

    void run()
    {
        std::string line;
        // Hold advertising until we know the node's name.
        (void)uart_send_line(uart_, "AT+MESHADV=OFF");
        (void)uart_send_line(uart_, "AT+MESHCLR");
        (void)uart_send_line(uart_, "AT+MESHSTATUS?");

        while(!g_stop) {
            struct pollfd pfd[2];
            int nfds = 1;
            pfd[0] = { uart_, POLLIN, 0 };
            if(api_.is_open()) {
                pfd[1] = { api_.fd(), POLLIN, 0 };
                nfds = 2;
            }
            int ret = poll(pfd, (nfds_t)nfds, 200);
            if(ret < 0 && errno != EINTR) {
                log_msg("poll failed: %s", strerror(errno));
                break;
            }
            if(ret > 0 && (pfd[0].revents & POLLIN)) {
                char buf[512];
                ssize_t n = read(uart_, buf, sizeof(buf));
                for(ssize_t i = 0; i < n; i++) {
                    if(buf[i] == '\n') {
                        handle_uart_line(line);
                        line.clear();
                    } else if(buf[i] != '\r' && line.size() < 4096) {
                        line.push_back(buf[i]);
                    }
                }
            }
            if(nfds == 2 && ret > 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                std::deque<std::vector<uint8_t>> frames;
                bool open = api_.read_frames(&frames);
                for(auto &f : frames) {
                    (void)owner_.feed(f);
                    fifo_.push_back(std::move(f));
                }
                if(!open) {
                    log_msg("meshtasticd closed the API connection");
                    api_.close_fd();
                }
                trim_fifo();
            }
            tick();
        }
        if(api_.is_open()) {
            (void)api_.send_frame(kToRadioDisconnect);
            api_.close_fd();
        }
        (void)uart_send_line(uart_, "AT+MESHADV=OFF");
        unlink(g_status_path);
    }

private:
    void tick()
    {
        uint64_t now = now_ms();

        if(name_.empty() && now >= next_name_try_ms_) {
            next_name_try_ms_ = now + kNameRetryMs;
            std::string name = g_name_override;
            if(!name.empty() || lookup_ble_name(&name)) {
                name_ = name;
                if(name_ != advertised_) {
                    log_msg("advertising as %s", name_.c_str());
                    advertised_ = name_;
                    write_status();
                }
                (void)uart_send_line(uart_, "AT+MESHADV=" + name_);
            } else if(!name_wait_logged_) {
                name_wait_logged_ = true;
                log_msg("waiting for meshtasticd on %s:%d", g_host, g_port);
            }
        }
        if(awaiting_ack_ && now - ack_sent_ms_ > kAckTimeoutMs) {
            log_msg("no reply to AT+MESHFROM; resending");
            awaiting_ack_ = false;
            (void)uart_send_line(uart_, "AT+MESHSTATUS?");
        }
        if(api_.is_open() && now - api_.last_tx_ms() > kHeartbeatMs) {
            (void)api_.send_frame(kToRadioHeartbeat);
        }
        pump();
    }

    void pump()
    {
        if(!connected_ || awaiting_ack_ || fifo_.empty() || nrf_queued_ >= kNrfQueueDepth) {
            return;
        }
        const std::vector<uint8_t> &f = fifo_.front();
        if(!uart_send_line(uart_, "AT+MESHFROM=" + hex_encode(f.data(), f.size()))) {
            log_msg("UART write failed: %s", strerror(errno));
            return;
        }
        awaiting_ack_ = true;
        ack_sent_ms_ = now_ms();
    }

    void write_status()
    {
        std::string tmp = std::string(g_status_path) + ".tmp";
        FILE *fp = fopen(tmp.c_str(), "w");
        if(!fp) return;
        fprintf(fp, "name=%s\nphone=%s\npin=%s\n", advertised_.c_str(),
                connected_ ? "connected" : "idle", pin_.c_str());
        fclose(fp);
        rename(tmp.c_str(), g_status_path);
    }

    void trim_fifo()
    {
        while(fifo_.size() > kFifoMax) {
            fifo_.pop_front();
            dropped_++;
        }
    }

    bool ensure_api()
    {
        if(api_.is_open()) return true;
        if(!api_.open_fd()) {
            log_msg("can't reach meshtasticd at %s:%d: %s", g_host, g_port, strerror(errno));
            return false;
        }
        log_msg("API session opened");
        return true;
    }

    void on_connected(const std::string &line)
    {
        connected_ = true;
        awaiting_ack_ = false;
        nrf_queued_ = 0;
        fifo_.clear();
        (void)uart_send_line(uart_, "AT+MESHCLR");
        log_msg("phone connected (%s)", line.c_str());
        write_status();
        (void)ensure_api();
    }

    void on_disconnected(const std::string &line)
    {
        log_msg("phone disconnected (%s); %zu queued, %zu dropped", line.c_str(),
                fifo_.size(), dropped_);
        connected_ = false;
        awaiting_ack_ = false;
        nrf_queued_ = 0;
        fifo_.clear();
        dropped_ = 0;
        pin_.clear();
        if(api_.is_open()) {
            (void)api_.send_frame(kToRadioDisconnect);
            api_.close_fd();
            log_msg("API session closed");
        }
        (void)uart_send_line(uart_, "AT+MESHCLR");
        // The phone may have renamed the node; re-advertise under the new name.
        std::string sniffed = owner_.ble_name();
        if(g_name_override.empty() && !sniffed.empty() && sniffed != advertised_) {
            name_ = advertised_ = sniffed;
            log_msg("advertising as %s", name_.c_str());
            (void)uart_send_line(uart_, "AT+MESHADV=" + name_);
        }
        write_status();
    }

    void on_toradio(const char *args)
    {
        char *end = nullptr;
        unsigned long len = strtoul(args, &end, 10);
        std::vector<uint8_t> payload;

        if(!end || *end != ',' || !hex_decode(end + 1, strlen(end + 1), &payload) ||
           payload.size() != len) {
            log_msg("bad TORADIO line (declared %lu bytes)", len);
            return;
        }
        if(!connected_) {
            // The write can beat +MESH:CONNECTED; treat it as a connect.
            on_connected("implicit");
        }
        if(!ensure_api()) return;
        if(!api_.send_frame(payload)) {
            log_msg("API write failed: %s", strerror(errno));
            api_.close_fd();
        }
    }

    void on_passkey(const char *args)
    {
        // +MESH:PASSKEY,<handle>,<code>,<match>
        const char *code = strchr(args, ',');
        pin_ = code ? std::string(code + 1, strcspn(code + 1, ",")) : "";
        log_msg("*** BLE pairing PIN: %s ***", pin_.c_str());
        write_status();
    }

    void handle_event(const std::string &ev)
    {
        if(ev.rfind("+MESH:TORADIO,", 0) == 0) {
            on_toradio(ev.c_str() + strlen("+MESH:TORADIO,"));
        } else if(ev.rfind("+MESH:FROM,", 0) == 0) {
            // +MESH:FROM,<fromNum>,<len>,<queued>
            const char *q = strrchr(ev.c_str(), ',');
            nrf_queued_ = q ? atoi(q + 1) : nrf_queued_;
            if(awaiting_ack_ && !fifo_.empty()) fifo_.pop_front();
            awaiting_ack_ = false;
        } else if(ev.rfind("+MESH:FROMREAD,", 0) == 0) {
            const char *q = strrchr(ev.c_str(), ',');
            nrf_queued_ = q ? atoi(q + 1) : nrf_queued_;
        } else if(ev.rfind("+MESH:CONNECTED", 0) == 0) {
            on_connected(ev);
        } else if(ev.rfind("+MESH:DISCONNECTED", 0) == 0) {
            if(connected_) on_disconnected(ev);
        } else if(ev.rfind("+MESH:PASSKEY,", 0) == 0) {
            on_passkey(ev.c_str() + strlen("+MESH:PASSKEY,"));
        } else if(ev.rfind("+MESH:STATUS,", 0) == 0) {
            // +MESH:STATUS,ADV=1,CONN=0,HANDLE=..,FROMNUM=..,QUEUE=n/8
            const char *q = strstr(ev.c_str(), "QUEUE=");
            if(q) nrf_queued_ = atoi(q + 6);
            bool conn = strstr(ev.c_str(), "CONN=1") != nullptr;
            if(conn && !connected_) on_connected(ev);
            if(!conn && connected_) on_disconnected(ev);
            log_msg("nRF %s", ev.c_str());
        } else if(ev.rfind("+MESH:PAIR,", 0) == 0 || ev.rfind("+MESH:SECURED,", 0) == 0) {
            log_msg("nRF %s", ev.c_str());
            if(!pin_.empty()) {
                pin_.clear();
                write_status();
            }
        }
    }

    void handle_uart_line(const std::string &raw)
    {
        if(raw.empty() || raw == "OK") return;
        if(raw.rfind("ERR:MESH_QUEUE", 0) == 0) {
            // Our count was stale; wait for a FROMREAD before retrying.
            awaiting_ack_ = false;
            nrf_queued_ = kNrfQueueDepth;
            return;
        }
        if(raw.rfind("ERR", 0) == 0 || raw.rfind("+ERR", 0) == 0) {
            log_msg("nRF %s", raw.c_str());
            if(awaiting_ack_ && !fifo_.empty()) {
                log_msg("dropping a %zu-byte FromRadio the nRF rejected", fifo_.front().size());
                fifo_.pop_front();
            }
            awaiting_ack_ = false;
            return;
        }
        // A line can carry more than one event if the nRF interleaves output.
        size_t pos = raw.find("+MESH:");
        while(pos != std::string::npos) {
            size_t next = raw.find("+MESH:", pos + 6);
            handle_event(raw.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
            pos = next;
        }
    }

    int uart_;
    ApiStream api_;
    OwnerInfo owner_;
    std::deque<std::vector<uint8_t>> fifo_;
    std::string name_;
    std::string advertised_;
    std::string pin_;
    bool name_wait_logged_ = false;
    uint64_t next_name_try_ms_ = 0;
    bool connected_ = false;
    bool awaiting_ack_ = false;
    uint64_t ack_sent_ms_ = 0;
    int nrf_queued_ = 0;
    size_t dropped_ = 0;
};

void on_signal(int)
{
    g_stop = 1;
}

void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--uart /dev/ttyS1] [--host 127.0.0.1] [--port 4403] [--name NAME]\n",
            argv0);
}

}  // namespace

int main(int argc, char **argv)
{
    for(int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if(a == "--uart" && i + 1 < argc) g_uart_path = argv[++i];
        else if(a == "--host" && i + 1 < argc) g_host = argv[++i];
        else if(a == "--port" && i + 1 < argc) g_port = atoi(argv[++i]);
        else if(a == "--name" && i + 1 < argc) g_name_override = argv[++i];
        else {
            usage(argv[0]);
            return 2;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    int uart = uart_open(g_uart_path);
    if(uart < 0) {
        log_msg("can't open %s: %s", g_uart_path, strerror(errno));
        return 1;
    }
    log_msg("bridging %s <-> %s:%d", g_uart_path, g_host, g_port);
    Bridge(uart).run();
    close(uart);
    log_msg("stopped");
    return 0;
}
