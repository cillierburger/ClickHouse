#include <IO/WriteBufferFromFileDirectIO.h>

#include <Common/Exception.h>
#include <Common/ErrnoException.h>
#include <Common/ProfileEvents.h>
#include <Common/CurrentMetrics.h>
#include <Common/Stopwatch.h>

#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace ProfileEvents
{
    extern const Event WriteBufferFromFileDescriptorWrite;
    extern const Event WriteBufferFromFileDescriptorWriteFailed;
    extern const Event WriteBufferFromFileDescriptorWriteBytes;
    extern const Event DiskWriteElapsedMicroseconds;
    extern const Event FileOpen;
}

namespace CurrentMetrics
{
    extern const Metric Write;
    extern const Metric OpenFileForWrite;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_OPEN_FILE;
    extern const int CANNOT_WRITE_TO_FILE_DESCRIPTOR;
    extern const int CANNOT_TRUNCATE_FILE;
}

WriteBufferFromFileDirectIO::WriteBufferFromFileDirectIO(
    const std::string & file_name_,
    size_t buf_size,
    ThrottlerPtr throttler_)
    : WriteBufferFromFileDescriptor(
        /* fd= */ -1,
        /* buf_size= */ alignUp(buf_size, DIRECT_IO_ALIGNMENT),
        /* existing_memory= */ nullptr,
        /* throttler_= */ throttler_,
        /* alignment= */ DIRECT_IO_ALIGNMENT,
        /* file_name_= */ file_name_)
{
    ProfileEvents::increment(ProfileEvents::FileOpen);

    fd = ::open(file_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT | O_CLOEXEC, 0666);
    if (fd == -1)
        ErrnoException::throwFromPath(
            ErrorCodes::CANNOT_OPEN_FILE, file_name, "Cannot open file {} for O_DIRECT writing", file_name);
}

WriteBufferFromFileDirectIO::~WriteBufferFromFileDirectIO()
{
    if (fd >= 0)
    {
        try
        {
            finalize();
        }
        catch (...)
        {
            tryLogCurrentException(__PRETTY_FUNCTION__);
        }

        ::close(fd);
        fd = -1;
    }
}

void WriteBufferFromFileDirectIO::nextImpl()
{
    size_t data_size = offset();
    if (!data_size)
        return;

    Stopwatch watch;

    size_t padded_size = alignUp(data_size, DIRECT_IO_ALIGNMENT);

    /// Zero-pad the tail of the buffer so the kernel sees an aligned write.
    if (padded_size > data_size)
        memset(working_buffer.begin() + data_size, 0, padded_size - data_size);

    size_t bytes_written = 0;
    while (bytes_written < padded_size)
    {
        ProfileEvents::increment(ProfileEvents::WriteBufferFromFileDescriptorWrite);

        ssize_t res = 0;
        {
            CurrentMetrics::Increment metric_increment{CurrentMetrics::Write};
            res = ::write(fd, working_buffer.begin() + bytes_written, padded_size - bytes_written);
        }

        if ((-1 == res || 0 == res) && errno != EINTR)
        {
            ProfileEvents::increment(ProfileEvents::WriteBufferFromFileDescriptorWriteFailed);
            ErrnoException::throwFromPath(
                ErrorCodes::CANNOT_WRITE_TO_FILE_DESCRIPTOR, file_name,
                "Cannot write to file {} (O_DIRECT)", file_name);
        }

        if (res > 0)
        {
            bytes_written += res;
            if (throttler)
                throttler->throttle(res);
        }
    }

    /// Track actual (unpadded) bytes for the final ftruncate.
    actual_bytes_written += data_size;

    ProfileEvents::increment(ProfileEvents::DiskWriteElapsedMicroseconds, watch.elapsedMicroseconds());
    /// Report real data bytes, not padded, so metrics stay consistent.
    ProfileEvents::increment(ProfileEvents::WriteBufferFromFileDescriptorWriteBytes, data_size);
}

void WriteBufferFromFileDirectIO::finalizeImpl()
{
    /// Flush whatever is left in the buffer (will be padded inside nextImpl).
    next();

    /// Remove the zero-padding added to satisfy O_DIRECT alignment on the last
    /// block. Without this the file would be up to DIRECT_IO_ALIGNMENT-1 bytes
    /// longer than the actual data.
    if (::ftruncate(fd, static_cast<off_t>(actual_bytes_written)) == -1)
        ErrnoException::throwFromPath(
            ErrorCodes::CANNOT_TRUNCATE_FILE, file_name,
            "Cannot truncate file {} after O_DIRECT write", file_name);
}

}
