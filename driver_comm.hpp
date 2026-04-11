#pragma once
#include "stdafx.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define DEVICE_NAME "\\\\.\\MyPortIODev"

#define IOCTL_MYPORTIO_READ_PHYS   0x9C406680
#define IOCTL_MYPORTIO_WRITE_PHYS  0x9C40A684

#pragma pack(push, 1)
struct MYPORTIO_PHYS_READ_REQ
{
    uint64_t PhysAddr;       // PHYSICAL_ADDRESS
    DWORD NumberOfBytes;     // MyPortIO physical IO path only handles a DWORD.
};

struct MYPORTIO_PHYS_WRITE_REQ
{
    uint64_t PhysAddr;       // PHYSICAL_ADDRESS
    DWORD NumberOfBytes;     // MyPortIO physical IO path only handles a DWORD.
    DWORD Value;             // DWORD to write
};
#pragma pack(pop)

static_assert(sizeof(MYPORTIO_PHYS_READ_REQ) == 12, "bad read req size");
static_assert(sizeof(MYPORTIO_PHYS_WRITE_REQ) == 16, "bad write req size");

inline constexpr DWORD MYPORTIO_DWORD_TRANSFER_SIZE = sizeof(DWORD);
inline HANDLE g_hDevice = nullptr;

inline bool ValidatePhysDwordIo(HANDLE hDevice, uint64_t offset, const char* operation)
{
    if (hDevice == nullptr || hDevice == INVALID_HANDLE_VALUE)
    {
        printf("%s failed: invalid device handle\n", operation);
        return false;
    }

    if (offset > UINT64_MAX - static_cast<uint64_t>(MYPORTIO_DWORD_TRANSFER_SIZE - 1))
    {
        printf("%s failed: physical range wraps. offset=0x%016llX size=%lu\n",
            operation,
            static_cast<unsigned long long>(offset),
            MYPORTIO_DWORD_TRANSFER_SIZE);
        return false;
    }

    return true;
}

inline bool OpenDriverDevice()
{
    HANDLE hDevice = CreateFileA(
        DEVICE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        printf("Failed to open device. gle=%lu\n", GetLastError());
        return false;
    }
    g_hDevice = hDevice;
    return true;
}

inline bool WritePhysMemory4B(HANDLE hDevice, uint64_t offset, DWORD value)
{
    if (!ValidatePhysDwordIo(hDevice, offset, "WritePhysMemory4B"))
    {
        return false;
    }

    MYPORTIO_PHYS_WRITE_REQ req = {};
    req.PhysAddr = offset;
    req.NumberOfBytes = MYPORTIO_DWORD_TRANSFER_SIZE;
    req.Value = value;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_MYPORTIO_WRITE_PHYS,
        &req,
        sizeof(req),
        nullptr,
        0,
        &bytesReturned,
        nullptr
    );

    if (!ok)
    {
        printf("WritePhysMemory failed. offset=0x%016llX value=0x%08X gle=%lu\n",
            static_cast<unsigned long long>(offset), value, GetLastError());
        return false;
    }

    return true;
}

inline bool ReadPhysMemory4B(HANDLE hDevice, uint64_t offset, DWORD& value)
{
    value = 0;

    if (!ValidatePhysDwordIo(hDevice, offset, "ReadPhysMemory4B"))
    {
        return false;
    }

    MYPORTIO_PHYS_READ_REQ req = {};
    req.PhysAddr = offset;
    req.NumberOfBytes = MYPORTIO_DWORD_TRANSFER_SIZE;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_MYPORTIO_READ_PHYS,
        &req,
        sizeof(req),
        &req,
        sizeof(req),
        &bytesReturned,
        nullptr
    );

    if (!ok)
    {
        printf("ReadPhysMemory failed. offset=0x%016llX gle=%lu\n",
            static_cast<unsigned long long>(offset), GetLastError());
        return false;
    }

    if (bytesReturned < MYPORTIO_DWORD_TRANSFER_SIZE)
    {
        printf("ReadPhysMemory failed. offset=0x%016llX returned=%lu expected=%lu\n",
            static_cast<unsigned long long>(offset),
            bytesReturned,
            MYPORTIO_DWORD_TRANSFER_SIZE);
        return false;
    }

    // MyPortIO writes the returned DWORD into the first four bytes of SystemBuffer.
    value = static_cast<DWORD>(req.PhysAddr & 0xFFFFFFFF);
    return true;
}

inline bool ReadPhysMemory(HANDLE hDevice, uint64_t offset, void* buffer, size_t size)
{
    if (buffer == nullptr && size != 0)
    {
        printf("ReadPhysMemory failed: null output buffer\n");
        return false;
    }

    uint8_t* pDest = static_cast<uint8_t*>(buffer);
    size_t bytesRead = 0;

    while (bytesRead < size)
    {
        DWORD chunk = 0;
        size_t remaining = size - bytesRead;

        if (bytesRead > UINT64_MAX - offset)
        {
            printf("ReadPhysMemory failed: physical range wraps. offset=0x%016llX size=%zu\n",
                static_cast<unsigned long long>(offset),
                size);
            return false;
        }

        uint64_t currentOffset = offset + bytesRead;
        if (!ReadPhysMemory4B(hDevice, currentOffset, chunk))
        {
            return false;
        }

        if (remaining >= MYPORTIO_DWORD_TRANSFER_SIZE)
        {
            memcpy(pDest + bytesRead, &chunk, MYPORTIO_DWORD_TRANSFER_SIZE);
            bytesRead += MYPORTIO_DWORD_TRANSFER_SIZE;
        }
        else
        {
            memcpy(pDest + bytesRead, &chunk, remaining);
            bytesRead += remaining;
        }
    }

    return true;
}

inline bool WritePhysMemory(HANDLE hDevice, uint64_t offset, const void* buffer, size_t size)
{
    if (buffer == nullptr && size != 0)
    {
        printf("WritePhysMemory failed: null input buffer\n");
        return false;
    }

    const uint8_t* pSrc = static_cast<const uint8_t*>(buffer);
    size_t bytesWritten = 0;

    while (bytesWritten < size)
    {
        size_t remaining = size - bytesWritten;

        if (bytesWritten > UINT64_MAX - offset)
        {
            printf("WritePhysMemory failed: physical range wraps. offset=0x%016llX size=%zu\n",
                static_cast<unsigned long long>(offset),
                size);
            return false;
        }

        uint64_t currentOffset = offset + bytesWritten;

        if (remaining >= MYPORTIO_DWORD_TRANSFER_SIZE)
        {
            DWORD chunk = 0;
            memcpy(&chunk, pSrc + bytesWritten, MYPORTIO_DWORD_TRANSFER_SIZE);
            if (!WritePhysMemory4B(hDevice, currentOffset, chunk))
            {
                return false;
            }
            bytesWritten += MYPORTIO_DWORD_TRANSFER_SIZE;
        }
        else
        {
            DWORD chunk = 0;
            if (!ReadPhysMemory4B(hDevice, currentOffset, chunk))
            {
                return false;
            }
            memcpy(&chunk, pSrc + bytesWritten, remaining);
            if (!WritePhysMemory4B(hDevice, currentOffset, chunk))
            {
                return false;
            }
            bytesWritten += remaining;
        }
    }

    return true;
}

template <typename T>
inline bool ReadPhysType(HANDLE hDevice, uint64_t offset, T& outData)
{
    return ReadPhysMemory(hDevice, offset, &outData, sizeof(T));
}

template <typename T>
inline bool WritePhysType(HANDLE hDevice, uint64_t offset, const T& data)
{
    return WritePhysMemory(hDevice, offset, &data, sizeof(T));
}
