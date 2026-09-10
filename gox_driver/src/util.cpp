#include "util.h"

#include <fstream>
#include <random>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

#include "time_util.h"


namespace gox {
    void PublishMetadata(const std::string &path, const std::string &text) {
        const auto fail = [](const std::string &what) {
            throw std::runtime_error(what + ": " + std::strerror(errno));
        };
        const std::string part = path + ".part";
        int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) fail("create " + part);
        int dir_fd = -1;
        try {
            std::size_t done = 0;
            while (done < text.size()) {
                const auto n = ::write(fd, text.data() + done, text.size() - done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    if (n == 0) errno = EIO;
                    fail("write " + part);
                }
                done += static_cast<std::size_t>(n);
            }
            int synced;
            do { synced = ::fdatasync(fd); } while (synced < 0 && errno == EINTR);
            if (synced != 0) fail("sync " + part);
            const int closed = ::close(fd);
            fd = -1; // close must not be retried on Linux, including EINTR
            if (closed != 0) fail("close " + part);
            // link() is an atomic no-replace publication in the same directory.
            if (::link(part.c_str(), path.c_str()) != 0) fail("publish " + path);
            if (::unlink(part.c_str()) != 0) fail("remove " + part);
            auto parent = std::filesystem::path(path).parent_path();
            if (parent.empty()) parent = ".";
            dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dir_fd < 0) fail("open metadata directory");
            do { synced = ::fsync(dir_fd); } while (synced < 0 && errno == EINTR);
            if (synced != 0) fail("sync metadata directory");
            ::close(dir_fd);
        } catch (...) {
            if (fd >= 0) ::close(fd);
            if (dir_fd >= 0) ::close(dir_fd);
            ::unlink(part.c_str()); // only our exclusively created temporary file
            throw;
        }
    }

    void GenUuidV4(uint8_t out[16]) {
        std::ifstream urandom("/dev/urandom", std::ios::binary);
        if (urandom.read(reinterpret_cast<char *>(out), 16) && urandom.gcount() == 16) {
            // ok
        } else {
            // Fallback: seeded PRNG. Only reached on systems without /dev/urandom.
            std::mt19937_64 rng(common::TimeUtil::RealtimeNowNs() ^ common::TimeUtil::MonotonicNowNs());
            for (int i = 0; i < 16; i += 8) {
                uint64_t v = rng();
                for (int j = 0; j < 8; ++j) {
                    out[i + j] = static_cast<uint8_t>(v >> (j * 8));
                }
            }
        }
        out[6] = static_cast<uint8_t>((out[6] & 0x0F) | 0x40); // version 4
        out[8] = static_cast<uint8_t>((out[8] & 0x3F) | 0x80); // variant 10xx
    }

    std::string HexPrefix(const uint8_t *data, size_t n) {
        static const char *digits = "0123456789abcdef";
        std::string s;
        s.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) {
            s.push_back(digits[data[i] >> 4]);
            s.push_back(digits[data[i] & 0x0F]);
        }
        return s;
    }
} // namespace gox
