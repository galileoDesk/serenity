/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//#define PATA_DEVICE_DEBUG

#include <AK/Memory.h>
#include <AK/StringView.h>
#include <Kernel/Devices/PATAChannel.h>
#include <Kernel/Devices/PATADiskDevice.h>
#include <Kernel/FileSystem/FileDescription.h>

namespace Kernel {

NonnullRefPtr<PATADiskDevice> PATADiskDevice::create(PATAChannel& channel, DriveType type, int major, int minor)
{
    return adopt(*new PATADiskDevice(channel, type, major, minor));
}

PATADiskDevice::PATADiskDevice(PATAChannel& channel, DriveType type, int major, int minor)
    : BlockDevice(major, minor, 512)
    , m_drive_type(type)
    , m_channel(channel)
{
}

PATADiskDevice::~PATADiskDevice()
{
}

const char* PATADiskDevice::class_name() const
{
    return "PATADiskDevice";
}

void PATADiskDevice::start_request(AsyncBlockDeviceRequest& request)
{
    bool use_dma = !m_channel.m_bus_master_base.is_null() && m_channel.m_dma_enabled.resource();
    m_channel.start_request(request, use_dma, is_slave());
}

void PATADiskDevice::set_drive_geometry(u16 cyls, u16 heads, u16 spt)
{
    m_cylinders = cyls;
    m_heads = heads;
    m_sectors_per_track = spt;
}

KResultOr<size_t> PATADiskDevice::read(FileDescription&, size_t offset, UserOrKernelBuffer& outbuf, size_t len)
{
    unsigned index = offset / block_size();
    u16 whole_blocks = len / block_size();
    ssize_t remaining = len % block_size();

    unsigned blocks_per_page = PAGE_SIZE / block_size();

    // PATAChannel will chuck a wobbly if we try to read more than PAGE_SIZE
    // at a time, because it uses a single page for its DMA buffer.
    if (whole_blocks >= blocks_per_page) {
        whole_blocks = blocks_per_page;
        remaining = 0;
    }

#ifdef PATA_DEVICE_DEBUG
    klog() << "PATADiskDevice::read() index=" << index << " whole_blocks=" << whole_blocks << " remaining=" << remaining;
#endif

    if (whole_blocks > 0) {
        auto read_request = make_request<AsyncBlockDeviceRequest>(AsyncBlockDeviceRequest::Read, index, whole_blocks, outbuf, whole_blocks * block_size());
        auto result = read_request->wait();
        if (result.wait_result().was_interrupted())
            return KResult(-EINTR);
        switch (result.request_result()) {
        case AsyncDeviceRequest::Failure:
        case AsyncDeviceRequest::Cancelled:
            return KResult(-EIO);
        case AsyncDeviceRequest::MemoryFault:
            return KResult(-EFAULT);
        default:
            break;
        }
    }

    off_t pos = whole_blocks * block_size();

    if (remaining > 0) {
        auto data = ByteBuffer::create_uninitialized(block_size());
        auto data_buffer = UserOrKernelBuffer::for_kernel_buffer(data.data());
        auto read_request = make_request<AsyncBlockDeviceRequest>(AsyncBlockDeviceRequest::Read, index + whole_blocks, 1, data_buffer, block_size());
        auto result = read_request->wait();
        if (result.wait_result().was_interrupted())
            return KResult(-EINTR);
        switch (result.request_result()) {
        case AsyncDeviceRequest::Failure:
            return pos;
        case AsyncDeviceRequest::Cancelled:
            return KResult(-EIO);
        case AsyncDeviceRequest::MemoryFault:
            // This should never happen, we're writing to a kernel buffer!
            ASSERT_NOT_REACHED();
        default:
            break;
        }
        if (!outbuf.write(data.data(), pos, remaining))
            return KResult(-EFAULT);
    }

    return pos + remaining;
}

bool PATADiskDevice::can_read(const FileDescription&, size_t offset) const
{
    return offset < (m_cylinders * m_heads * m_sectors_per_track * block_size());
}

KResultOr<size_t> PATADiskDevice::write(FileDescription&, size_t offset, const UserOrKernelBuffer& inbuf, size_t len)
{
    unsigned index = offset / block_size();
    u16 whole_blocks = len / block_size();
    ssize_t remaining = len % block_size();

    unsigned blocks_per_page = PAGE_SIZE / block_size();

    // PATAChannel will chuck a wobbly if we try to write more than PAGE_SIZE
    // at a time, because it uses a single page for its DMA buffer.
    if (whole_blocks >= blocks_per_page) {
        whole_blocks = blocks_per_page;
        remaining = 0;
    }

#ifdef PATA_DEVICE_DEBUG
    klog() << "PATADiskDevice::write() index=" << index << " whole_blocks=" << whole_blocks << " remaining=" << remaining;
#endif

    if (whole_blocks > 0) {
        auto write_request = make_request<AsyncBlockDeviceRequest>(AsyncBlockDeviceRequest::Write, index, whole_blocks, inbuf, whole_blocks * block_size());
        auto result = write_request->wait();
        if (result.wait_result().was_interrupted())
            return KResult(-EINTR);
        switch (result.request_result()) {
        case AsyncDeviceRequest::Failure:
        case AsyncDeviceRequest::Cancelled:
            return KResult(-EIO);
        case AsyncDeviceRequest::MemoryFault:
            return KResult(-EFAULT);
        default:
            break;
        }
    }

    off_t pos = whole_blocks * block_size();

    // since we can only write in block_size() increments, if we want to do a
    // partial write, we have to read the block's content first, modify it,
    // then write the whole block back to the disk.
    if (remaining > 0) {
        auto data = ByteBuffer::create_zeroed(block_size());
        auto data_buffer = UserOrKernelBuffer::for_kernel_buffer(data.data());

        {
            auto read_request = make_request<AsyncBlockDeviceRequest>(AsyncBlockDeviceRequest::Read, index + whole_blocks, 1, data_buffer, block_size());
            auto result = read_request->wait();
            if (result.wait_result().was_interrupted())
                return KResult(-EINTR);
            switch (result.request_result()) {
            case AsyncDeviceRequest::Failure:
                return pos;
            case AsyncDeviceRequest::Cancelled:
                return KResult(-EIO);
            case AsyncDeviceRequest::MemoryFault:
                // This should never happen, we're writing to a kernel buffer!
                ASSERT_NOT_REACHED();
            default:
                break;
            }
        }

        if (!inbuf.read(data.data(), pos, remaining))
            return KResult(-EFAULT);

        {
            auto write_request = make_request<AsyncBlockDeviceRequest>(AsyncBlockDeviceRequest::Write, index + whole_blocks, 1, data_buffer, block_size());
            auto result = write_request->wait();
            if (result.wait_result().was_interrupted())
                return KResult(-EINTR);
            switch (result.request_result()) {
            case AsyncDeviceRequest::Failure:
                return pos;
            case AsyncDeviceRequest::Cancelled:
                return KResult(-EIO);
            case AsyncDeviceRequest::MemoryFault:
                // This should never happen, we're writing to a kernel buffer!
                ASSERT_NOT_REACHED();
            default:
                break;
            }
        }
    }

    return pos + remaining;
}

bool PATADiskDevice::can_write(const FileDescription&, size_t offset) const
{
    return offset < (m_cylinders * m_heads * m_sectors_per_track * block_size());
}

bool PATADiskDevice::is_slave() const
{
    return m_drive_type == DriveType::Slave;
}

}
