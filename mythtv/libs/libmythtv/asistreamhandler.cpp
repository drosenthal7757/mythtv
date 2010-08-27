// -*- Mode: c++ -*-

// POSIX headers
#include <fcntl.h>
#include <unistd.h>
#ifndef USING_MINGW
#include <sys/select.h>
#include <sys/ioctl.h>
#endif

// Qt headers
#include <QString>
#include <QFile>

// MythTV headers
#include "asistreamhandler.h"
#include "asichannel.h"
#include "dtvsignalmonitor.h"
#include "streamlisteners.h"
#include "mpegstreamdata.h"
#include "cardutil.h"

// DVEO ASI headers
#include <dveo/asi.h>
#include <dveo/master.h>

#define LOC      QString("ASISH(%1): ").arg(_device)
#define LOC_WARN QString("ASISH(%1) Warning: ").arg(_device)
#define LOC_ERR  QString("ASISH(%1) Error: ").arg(_device)

QMap<QString,ASIStreamHandler*> ASIStreamHandler::_handlers;
QMap<QString,uint>              ASIStreamHandler::_handlers_refcnt;
QMutex                          ASIStreamHandler::_handlers_lock;

ASIStreamHandler *ASIStreamHandler::Get(const QString &devname)
{
    QMutexLocker locker(&_handlers_lock);

    QString devkey = devname.toUpper();

    QMap<QString,ASIStreamHandler*>::iterator it = _handlers.find(devkey);

    if (it == _handlers.end())
    {
        ASIStreamHandler *newhandler = new ASIStreamHandler(devkey);
        newhandler->Open();
        _handlers[devkey] = newhandler;
        _handlers_refcnt[devkey] = 1;

        VERBOSE(VB_RECORD,
                QString("ASISH: Creating new stream handler %1 for %2")
                .arg(devkey).arg(devname));
    }
    else
    {
        _handlers_refcnt[devkey]++;
        uint rcount = _handlers_refcnt[devkey];
        VERBOSE(VB_RECORD,
                QString("ASISH: Using existing stream handler %1 for %2")
                .arg(devkey)
                .arg(devname) + QString(" (%1 in use)").arg(rcount));
    }

    return _handlers[devkey];
}

void ASIStreamHandler::Return(ASIStreamHandler * & ref)
{
    QMutexLocker locker(&_handlers_lock);

    QString devname = ref->_device;

    QMap<QString,uint>::iterator rit = _handlers_refcnt.find(devname);
    if (rit == _handlers_refcnt.end())
        return;

    if (*rit > 1)
    {
        ref = NULL;
        (*rit)--;
        return;
    }

    QMap<QString,ASIStreamHandler*>::iterator it = _handlers.find(devname);
    if ((it != _handlers.end()) && (*it == ref))
    {
        VERBOSE(VB_RECORD, QString("ASISH: Closing handler for %1")
                           .arg(devname));
        ref->Close();
        delete *it;
        _handlers.erase(it);
    }
    else
    {
        VERBOSE(VB_IMPORTANT,
                QString("ASISH Error: Couldn't find handler for %1")
                .arg(devname));
    }

    _handlers_refcnt.erase(rit);
    ref = NULL;
}

ASIStreamHandler::ASIStreamHandler(const QString &device) :
    StreamHandler(device), _device_num(-1), _buf_size(-1), _fd(-1)
{
}

ASIStreamHandler::~ASIStreamHandler()
{
    if (!_stream_data_list.empty())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "dtor & _stream_data_list not empty");
    }
}

void ASIStreamHandler::run(void)
{
    VERBOSE(VB_RECORD, LOC + "run(): begin");

    if (!Open())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                QString("Failed to open device %1 : %2")
                .arg(_device).arg(strerror(errno)));
        _error = true;
        return;
    }

    // TODO use _buf_size...
    DeviceReadBuffer *drb = new DeviceReadBuffer(this);
    bool ok = drb->Setup(_device, _fd);
    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to allocate DRB buffer");
        delete drb;
        Close();
        _error = true;
        return;
    }

    int buffer_size = TSPacket::SIZE * 15000;
    unsigned char *buffer = new unsigned char[buffer_size];
    if (!buffer)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to allocate buffer");
        delete drb;
        Close();
        _error = true;
        return;
    }
    bzero(buffer, buffer_size);

    SetRunning(true, true, false);

    drb->Start();

    int remainder = 0;
    while (_running_desired && !_error)
    {
        UpdateFiltersFromStreamData();

        ssize_t len = 0;

        len = drb->Read(
            &(buffer[remainder]), buffer_size - remainder);

        // Check for DRB errors
        if (drb->IsErrored())
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "Device error detected");
            _error = true;
        }

        if (drb->IsEOF())
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "Device EOF detected");
            _error = true;
        }

        if ((0 == len) || (-1 == len))
        {
            usleep(100);
            continue;
        }

        len += remainder;

        if (len < 10) // 10 bytes = 4 bytes TS header + 6 bytes PES header
        {
            remainder = len;
            continue;
        }

        _listener_lock.lock();

        if (_stream_data_list.empty())
        {
            _listener_lock.unlock();
            continue;
        }

        for (uint i = 0; i < _stream_data_list.size(); i++)
        {
            remainder = _stream_data_list[i]->ProcessData(buffer, len);
        }

        _listener_lock.unlock();

        if (remainder > 0 && (len > remainder)) // leftover bytes
            memmove(buffer, &(buffer[len - remainder]), remainder);
    }
    VERBOSE(VB_RECORD, LOC + "run(): " + "shutdown");

    RemoveAllPIDFilters();

    if (drb->IsRunning())
        drb->Stop();

    delete drb;
    delete[] buffer;
    Close();

    VERBOSE(VB_RECORD, LOC + "run(): " + "end");

    SetRunning(false, true, false);
}

bool ASIStreamHandler::Open(void)
{
    if (_fd >= 0)
        return true;

    QString error;
    _device_num = CardUtil::GetASIDeviceNumber(_device, &error);
    if (_device_num < 0)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + error);
        return false;
    }

    _buf_size = CardUtil::GetASIBufferSize(_device_num, &error);
    if (_buf_size <= 0)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + error);
        return false;
    }

    // actually open the device
    _fd = open(_device.toLocal8Bit().constData(), O_RDONLY, 0);
    if (_fd < 0)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                QString("Failed to open '%1'").arg(_device) + ENO);
        return false;
    }

    // get the rx capabilities
    unsigned int cap;
    if (ioctl(_fd, ASI_IOC_RXGETCAP, &cap) < 0)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                QString("Failed to query capabilities '%1'")
                .arg(_device) + ENO);
        Close();
        return false;
    }

    // TODO do stuff with capabilities..
    // we need to handle 188 & 204 byte packets..

    // pid counter?

    return false;
}

void ASIStreamHandler::Close(void)
{
    close(_fd);
    _fd = -1;
}

void ASIStreamHandler::PriorityEvent(int fd)
{
    // TODO report on buffer overruns, etc.
}
