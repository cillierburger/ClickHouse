#pragma once

#include <IO/WriteBufferFromFileDescriptor.h>

#ifndef O_DIRECT
#define O_DIRECT 00040000
#endif

namespace DB
{

/** WriteBuffer that opens a file with O_DIRECT and ensures every write()
  * syscall is aligned to DIRECT_IO_ALIGNMENT bytes, which is required by the
  * kernel for direct (non-buffered) I/O.
  *
  * Full-buffer flushes are always aligned because the buffer size is rounded up
  * to DIRECT_IO_ALIGNMENT at construction. The final partial flush is padded to
  * the next boundary with zeros; after all data is written, finalizeImpl()
  * calls ftruncate() to restore the file to its true (unpadded) length.
  *
  * The buffer is allocated with posix_memalign so its base address satisfies
  * the alignment requirement as well.
  *
  * Intended for INSERT part writes on local disks. Bypasses the page cache
  * entirely, which prevents dirty-page pressure when many concurrent INSERTs
  * are writing large volumes of data.
  */
class WriteBufferFromFileDirectIO final : public WriteBufferFromFileDescriptor
{
public:
    static constexpr size_t DIRECT_IO_ALIGNMENT = 4096;

    explicit WriteBufferFromFileDirectIO(
        const std::string & file_name_,
        size_t buf_size,
        ThrottlerPtr throttler_ = {});

    ~WriteBufferFromFileDirectIO() override;

    std::string getFileName() const override { return file_name; }

protected:
    void nextImpl() override;
    void finalizeImpl() override;

private:
    static size_t alignUp(size_t n, size_t alignment)
    {
        return (n + alignment - 1) & ~(alignment - 1);
    }

    size_t actual_bytes_written = 0;
};

}
