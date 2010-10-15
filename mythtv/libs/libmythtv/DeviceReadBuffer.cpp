#include <algorithm>
using namespace std;

#include "DeviceReadBuffer.h"
#include "mythcorecontext.h"
#include "mythverbose.h"
#include "mythdbutil.h"
#include "tspacket.h"
#include "compat.h"

#ifndef USING_MINGW
#include <sys/poll.h>
#endif

/// Set this to 1 to report on statistics
#define REPORT_RING_STATS 0

#define LOC QString("DevRdB(%1): ").arg(videodevice)
#define LOC_ERR QString("DevRdB(%1) Error: ").arg(videodevice)

DeviceReadBuffer::DeviceReadBuffer(DeviceReaderCB *cb, bool use_poll)
    : videodevice(QString::null),   _stream_fd(-1),
      readerCB(cb),

      // Data for managing the device ringbuffer
      run(false),                   running(false),
      eof(false),                   error(false),
      request_pause(false),         paused(false),
      using_poll(use_poll),         max_poll_wait(2500 /*ms*/),

      size(0),                      used(0),
      read_quanta(0),
      dev_read_size(0),             min_read(0),

      buffer(NULL),                 readPtr(NULL),
      writePtr(NULL),               endPtr(NULL),

      // statistics
      max_used(0),                  avg_used(0),
      avg_cnt(0)
{
    for (int i = 0; i < 2; i++)
    {
        wake_pipe[i] = -1;
        wake_pipe_flags[i] = 0;
    }

#ifdef USING_MINGW
#warning mingw DeviceReadBuffer::Poll
    if (using_poll)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                "mingw DeviceReadBuffer::Poll is not implemented");
        using_poll = false;
    }
#endif
}

DeviceReadBuffer::~DeviceReadBuffer()
{
    if (buffer)
        delete[] buffer;
}

bool DeviceReadBuffer::Setup(const QString &streamName, int streamfd,
                             uint readQuanta, uint deviceBufferSize)
{
    QMutexLocker locker(&lock);

    if (buffer)
        delete[] buffer;

    videodevice   = streamName;
    _stream_fd    = streamfd;

    // BEGIN HACK -- see #6897, remove after August 2009
    max_poll_wait = (videodevice.contains("dvb")) ? 25000 : 2500;
    // END HACK

    // Setup device ringbuffer
    eof           = false;
    error         = false;
    request_pause = false;
    paused        = false;

    read_quanta   = (readQuanta) ? readQuanta : read_quanta;
    size          = gCoreContext->GetNumSetting(
        "HDRingbufferSize", 50 * read_quanta) * 1024;
    used          = 0;
    dev_read_size = read_quanta * (using_poll ? 256 : 48);
    dev_read_size = (deviceBufferSize) ?
        min(dev_read_size, (size_t)deviceBufferSize) : dev_read_size;
    min_read      = read_quanta * 4;

    buffer        = new unsigned char[size + dev_read_size];
    readPtr       = buffer;
    writePtr      = buffer;
    endPtr        = buffer + size;

    // Initialize buffer, if it exists
    if (!buffer)
        return false;
    memset(buffer, 0xFF, size + read_quanta);

    // Initialize statistics
    max_used      = 0;
    avg_used      = 0;
    avg_cnt       = 0;
    lastReport.start();

    VERBOSE(VB_RECORD, LOC + QString("buffer size %1 KB").arg(size/1024));

    return true;
}

void DeviceReadBuffer::Start(void)
{
    bool was_running;

    {
        QMutexLocker locker(&lock);
        was_running = running;
        error = false;
    }

    if (was_running)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Start(): Already running.");
        SetRequestPause(false);
        return;
    }

    if (pthread_create(&thread, NULL, boot_ringbuffer, this) != 0)
    {
        VERBOSE(VB_IMPORTANT,
                LOC_ERR + QString("Start(): pthread_create failed.") + ENO);

        QMutexLocker locker(&lock);
        error = true;
    }
}

void DeviceReadBuffer::Reset(const QString &streamName, int streamfd)
{
    QMutexLocker locker(&lock);

    videodevice   = streamName;
    _stream_fd    = streamfd;

    used          = 0;
    readPtr       = buffer;
    writePtr      = buffer;

    error         = false;
}

void DeviceReadBuffer::Stop(void)
{
    bool was_running = IsRunning();

    if (!was_running)
    {
        VERBOSE(VB_IMPORTANT, LOC + "Stop(): Not running.");
        return;
    }

    {
        QMutexLocker locker(&lock);
        run = false;
    }

    WakePoll();

    pthread_join(thread, NULL);
}

void DeviceReadBuffer::SetRequestPause(bool req)
{
    QMutexLocker locker(&lock);
    request_pause = req;
    WakePoll();
}

void DeviceReadBuffer::SetPaused(bool val)
{
    QMutexLocker locker(&lock);
    paused = val;
    if (val)
        pauseWait.wakeAll();
    else
        unpauseWait.wakeAll();
}

// The WakePoll code is copied from MythSocketThread::WakeReadyReadThread()
void DeviceReadBuffer::WakePoll(void) const
{
    char buf[1];
    buf[0] = '0';
    ssize_t wret = 0;
    while (running && (wret <= 0) && (wake_pipe[1] >= 0))
    {
        wret = ::write(wake_pipe[1], &buf, 1);
        if ((wret < 0) && (EAGAIN != errno) && (EINTR != errno))
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "WakePoll failed.");
            ClosePipes();
            break;
        }
    }
}

void DeviceReadBuffer::ClosePipes(void) const
{
    for (uint i = 0; i < 2; i++)
    {
        if (wake_pipe[i] >= 0)
        {
            ::close(wake_pipe[i]);
            wake_pipe[i] = -1;
            wake_pipe_flags[i] = 0;
        }
    }
}

bool DeviceReadBuffer::IsPaused(void) const
{
    QMutexLocker locker(&lock);
    return paused;
}

bool DeviceReadBuffer::WaitForPaused(unsigned long timeout)
{
    QMutexLocker locker(&lock);

    if (!paused)
        pauseWait.wait(&lock, timeout);

    return paused;
}

bool DeviceReadBuffer::WaitForUnpause(unsigned long timeout)
{
    QMutexLocker locker(&lock);

    if (paused)
        unpauseWait.wait(&lock, timeout);

    return paused;
}

bool DeviceReadBuffer::IsPauseRequested(void) const
{
    QMutexLocker locker(&lock);
    return request_pause;
}

bool DeviceReadBuffer::IsRunning(void) const
{
    QMutexLocker locker(&lock);
    return running;
}

uint DeviceReadBuffer::GetUnused(void) const
{
    QMutexLocker locker(&lock);
    return size - used;
}

uint DeviceReadBuffer::GetUsed(void) const
{
    QMutexLocker locker(&lock);
    return used;
}

uint DeviceReadBuffer::GetContiguousUnused(void) const
{
    QMutexLocker locker(&lock);
    return endPtr - writePtr;
}

void DeviceReadBuffer::IncrWritePointer(uint len)
{
    QMutexLocker locker(&lock);
    used     += len;
    writePtr += len;
    writePtr  = (writePtr >= endPtr) ? buffer + (writePtr - endPtr) : writePtr;
#if REPORT_RING_STATS
    max_used = max(used, max_used);
    avg_used = ((avg_used * avg_cnt) + used) / ++avg_cnt;
#endif
}

void DeviceReadBuffer::IncrReadPointer(uint len)
{
    QMutexLocker locker(&lock);
    used    -= len;
    readPtr += len;
    readPtr  = (readPtr == endPtr) ? buffer : readPtr;
}

void *DeviceReadBuffer::boot_ringbuffer(void *arg)
{
    ((DeviceReadBuffer*) arg)->fill_ringbuffer();
    return NULL;
}

void DeviceReadBuffer::fill_ringbuffer(void)
{
    uint      errcnt = 0;

    lock.lock();
    run     = true;
    running = true;
    lock.unlock();

    if (using_poll)
        setup_pipe(wake_pipe, wake_pipe_flags);

    while (run)
    {
        if (!HandlePausing())
            continue;

        if (!IsOpen())
        {
            usleep(5000);
            continue;
        }

        if (using_poll && !Poll())
            continue;

        {
            QMutexLocker locker(&lock);
            if (error)
            {
                VERBOSE(VB_RECORD, LOC + "fill_ringbuffer: error state");
                break;
            }
        }

        // Limit read size for faster return from read
        size_t unused = (size_t) WaitForUnused(read_quanta);
        size_t read_size = min(dev_read_size, unused);

        // if read_size > 0 do the read...
        if (read_size)
        {
            ssize_t len = read(_stream_fd, writePtr, read_size);
            if (!CheckForErrors(len, read_size, errcnt))
            {
                if (errcnt > 5)
                    break;
                else
                    continue;
            }
            errcnt = 0;
            // if we wrote past the official end of the buffer, copy to start
            if (writePtr + len > endPtr)
                memcpy(endPtr, buffer, writePtr + len - endPtr);
            IncrWritePointer(len);
        }
    }

    ClosePipes();

    lock.lock();
    running = false;
    eof     = true;
    lock.unlock();
}

bool DeviceReadBuffer::HandlePausing(void)
{
    if (IsPauseRequested())
    {
        SetPaused(true);

        if (readerCB)
            readerCB->ReaderPaused(_stream_fd);

        usleep(5000);
        return false;
    }
    else if (IsPaused())
    {
        Reset(videodevice, _stream_fd);
        SetPaused(false);
    }
    return true;
}

bool DeviceReadBuffer::Poll(void) const
{
#ifdef USING_MINGW
    return false;
#else
    bool retval = true;
    MythTimer timer;
    timer.start();

    int poll_cnt = 1;
    struct pollfd polls[2];
    memset(polls, 0, sizeof(polls));

    polls[0].fd      = _stream_fd;
    polls[0].events  = POLLIN | POLLPRI;
    polls[0].revents = 0;

    if (wake_pipe[0] >= 0)
    {
        poll_cnt = 2;
        polls[1].fd      = wake_pipe[0];
        polls[1].events  = POLLIN;
        polls[1].revents = 0;
    }

    while (true)
    {
        polls[0].revents = 0;
        polls[1].revents = 0;
        poll_cnt = (wake_pipe[0] >= 0) ? poll_cnt : 1;

        int timeout = (1 == poll_cnt) ? 10 : -1;
        int ret = poll(polls, poll_cnt, timeout);

        if (polls[0].revents & (POLLHUP | POLLNVAL))
        {
            VERBOSE(VB_IMPORTANT, LOC + "poll error");
            error = true;
            return true;
        }

        if (!run || !IsOpen() || IsPauseRequested())
        {
            retval = false;
            break; // are we supposed to pause, stop, etc.
        }

        if (polls[0].revents & POLLPRI)
        {
            readerCB->PriorityEvent(polls[0].fd);
        }

        if (polls[0].revents & POLLIN)
        {
            if (ret > 0)
                break; // we have data to read :)
            else if (ret < 0)
            {
                if ((EOVERFLOW == errno))
                    break; // we have an error to handle

                if ((EAGAIN == errno) || (EINTR  == errno))
                    continue; // errors that tell you to try again

                usleep(2500 /*2.5 ms*/);
            }
            else //  ret == 0
            {
                if ((uint)timer.elapsed() > max_poll_wait)
                {
                    VERBOSE(VB_IMPORTANT, LOC_ERR + "Poll giving up");
                    QMutexLocker locker(&lock);
                    error = true;
                    return true;
                }
            }
        }

        // Clear out any pending pipe reads
        if ((poll_cnt > 1) && (polls[1].revents & POLLIN))
        {
            char dummy[128];
            int cnt = (wake_pipe_flags[0] & O_NONBLOCK) ? 128 : 1;
            (void) ::read(wake_pipe[0], dummy, cnt);
        }
    }
    return retval;
#endif //!USING_MINGW
}

bool DeviceReadBuffer::CheckForErrors(
    ssize_t len, size_t requested_len, uint &errcnt)
{
    if (len > (ssize_t)requested_len)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                "Driver is retruning bogus values on read");
        if (++errcnt > 5)
        {
            VERBOSE(VB_RECORD, LOC + "Too many errors.");
            QMutexLocker locker(&lock);
            error = true;
        }
        return false;
    }

#ifdef USING_MINGW
#warning mingw DeviceReadBuffer::CheckForErrors
    VERBOSE(VB_IMPORTANT, LOC_ERR +
            "mingw DeviceReadBuffer::CheckForErrors is not implemented");
    return false;
#else
    if (len < 0)
    {
        if (EINTR == errno)
            return false;
        if (EAGAIN == errno)
        {
            usleep(2500);
            return false;
        }
        if (EOVERFLOW == errno)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "Driver buffers overflowed");
            return false;
        }

        VERBOSE(VB_IMPORTANT, LOC_ERR +
                QString("Problem reading fd(%1)").arg(_stream_fd) + ENO);

        if (++errcnt > 5)
        {
            VERBOSE(VB_RECORD, LOC + "Too many errors.");
            QMutexLocker locker(&lock);
            error = true;
            return false;
        }

        usleep(500);
        return false;
    }
    else if (len == 0)
    {
        if (++errcnt > 5)
        {
            VERBOSE(VB_IMPORTANT, LOC +
                    QString("End-Of-File? fd(%1)").arg(_stream_fd));

            lock.lock();
            eof = true;
            lock.unlock();

            return false;
        }
        usleep(500);
        return false;
    }
    return true;
#endif
}

/** \fn DeviceReadBuffer::Read(unsigned char*, const uint)
 *  \brief Try to Read count bytes from into buffer
 *  \param buf    Buffer to put data in
 *  \param count  Number of bytes to attempt to read
 *  \return number of bytes actually read
 */
uint DeviceReadBuffer::Read(unsigned char *buf, const uint count)
{
    uint avail = WaitForUsed(min(count, (uint)min_read));
    size_t cnt = min(count, avail);

    if (!cnt)
        return 0;

    if (readPtr + cnt > endPtr)
    {
        // Process as two pieces
        size_t len = endPtr - readPtr;
        if (len)
        {
            memcpy(buf, readPtr, len);
            buf += len;
            IncrReadPointer(len);
        }
        if (cnt > len)
        {
            len = cnt - len;
            memcpy(buf, readPtr, len);
            IncrReadPointer(len);
        }
    }
    else
    {
        memcpy(buf, readPtr, cnt);
        IncrReadPointer(cnt);
    }

#if REPORT_RING_STATS
    ReportStats();
#endif

    return cnt;
}

/** \fn DeviceReadBuffer::WaitForUnused(uint) const
 *  \param needed Number of bytes we want to write
 *  \return bytes available for writing
 */
uint DeviceReadBuffer::WaitForUnused(uint needed) const
{
    size_t unused = GetUnused();

    if (unused > read_quanta)
    {
        while (unused < needed)
        {
            unused = GetUnused();
            if (IsPauseRequested() || !IsOpen() || !run)
                return 0;
            usleep(5000);
        }
        if (IsPauseRequested() || !IsOpen() || !run)
            return 0;
        unused = GetUnused();
    }

    return unused;
}

/** \fn DeviceReadBuffer::WaitForUsed(uint) const
 *  \param needed Number of bytes we want to read
 *  \return bytes available for reading
 */
uint DeviceReadBuffer::WaitForUsed(uint needed) const
{
    size_t avail = GetUsed();
    while ((needed > avail) && running)
    {
        {
            QMutexLocker locker(&lock);
            avail = used;
            if (request_pause || error || eof)
                return 0;
        }
        usleep(5000);
    }
    return avail;
}

void DeviceReadBuffer::ReportStats(void)
{
#if REPORT_RING_STATS
    if (lastReport.elapsed() > 20*1000 /* msg every 20 seconds */)
    {
        QMutexLocker locker(&lock);
        double rsize = 100.0 / size;
        QString msg  = QString("fill avg(%1%) ").arg(avg_used*rsize,3,'f',0);
        msg         += QString("fill max(%2%) ").arg(max_used*rsize,3,'f',0);
        msg         += QString("samples(%3)").arg(avg_cnt);

        avg_used    = 0;
        avg_cnt     = 0;
        max_used    = 0;
        lastReport.start();

        VERBOSE(VB_IMPORTANT, LOC + msg);
    }
#endif
}
