#ifndef BUFFERED_WRITER_H
#define BUFFERED_WRITER_H

#include "point_cloud_types.h"
#include "binary_writer.h"

#include <cstddef>
#include <cstdint>
#include <string>


struct BufferedWriterConfig {
    std::string output_path;
    size_t buffer_size = 8 * 1024 * 1024; // 8 MB default
    size_t status_interval_frames = 100;
};


// Buffered binary writer that accumulates point cloud data in a large memory
// buffer and flushes to disk using POSIX write() with partial-write handling.
//
// Delegates raw I/O to Common::BinaryWriter; this class adds the LIDARPCD
// file format framing (magic header + per-frame header) on top.
//
// NOT thread-safe: designed to be called exclusively from the writer thread.
// Thread safety between callback and writer is provided by ThreadSafeQueue.
class BufferedBinaryWriter {
public:
    explicit BufferedBinaryWriter(const BufferedWriterConfig &config);

    ~BufferedBinaryWriter() = default;

    BufferedBinaryWriter(const BufferedBinaryWriter &) = delete;

    BufferedBinaryWriter &operator=(const BufferedBinaryWriter &) = delete;

    bool open();

    bool write_frame(const PointCloudFrame &frame);

    void close();

    bool is_open() const;

    WriterStats stats() const;

private:
    BufferedWriterConfig config_;
    Common::BinaryWriter writer_;
    WriterStats stats_;
};


#endif // BUFFERED_WRITER_H