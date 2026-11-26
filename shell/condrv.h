#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")

namespace condrv {

enum Function : ULONG { CONNECT = 1, DISCONNECT, CREATE_OBJECT, CLOSE_OBJECT, RAW_WRITE, RAW_READ, USER_DEFINED, RAW_FLUSH };
enum ObjectType : ULONG { CURRENT_INPUT = 1, CURRENT_OUTPUT, NEW_OUTPUT, GENERIC };

constexpr NTSTATUS SUCCESS = 0, ALERTED = 0x101, UNSUCCESSFUL = (NTSTATUS)0xC0000001, NOT_IMPLEMENTED = (NTSTATUS)0xC0000002,
                   INVALID_HANDLE = (NTSTATUS)0xC0000008, INVALID_PARAMETER = (NTSTATUS)0xC000000D,
                   NOT_SUPPORTED = (NTSTATUS)0xC00000BB, ILLEGAL_FUNCTION = (NTSTATUS)0xC00000AF, BUFFER_TOO_SMALL = (NTSTATUS)0xC0000023;

struct Descriptor {
    LUID id;
    ULONG_PTR process, object;
    ULONG function, inputSize, outputSize, reserved;
};

struct Buffer {
    const void* data;
    ULONG size, offset;
};

struct Completion {
    LUID id;
    IO_STATUS_BLOCK status;
    Buffer write;
};

struct Operation {
    LUID id;
    Buffer buffer;
};

struct Connection {
    ULONG_PTR process, input, output;
};

struct ConnectData {
    ULONG iconId, hotKey, startupFlags;
    USHORT fillAttribute, showWindow;
    COORD bufferSize, windowSize, windowOrigin;
    ULONG processGroup;
    BOOLEAN consoleApp, windowVisible;
    USHORT titleLength;
    WCHAR title[MAX_PATH + 1];
    USHORT appNameLength;
    WCHAR appName[128];
    USHORT curDirLength;
    WCHAR curDir[MAX_PATH + 1];
};

struct CreateObject {
    ULONG type, shareMode;
    ACCESS_MASK access;
};

constexpr ULONG BODY_BYTES = 256;

struct Message {
    Descriptor d;
    union {
        CreateObject create;
        struct {
            ULONG api, apiSize;
            BYTE body[BODY_BYTES];
        };
    };
    template <class T> T& as() { return *reinterpret_cast<T*>(body); }
    ULONG layer() const { return api >> 24; }
    ULONG call() const { return api & 0xFFFFFF; }
    ULONG userInput() const { return d.inputSize - apiSize - 8; }
};

struct PseudoConsole {
    HANDLE signal, reference, process;
};

constexpr DWORD control(DWORD fn, DWORD method) { return 0x50u << 16 | fn << 2 | method; }
constexpr DWORD READ_IO = control(1, 2), COMPLETE_IO = control(2, 3), READ_INPUT = control(3, 3), WRITE_OUTPUT = control(4, 3),
                SET_SERVER_INFORMATION = control(7, 3);
constexpr DWORD PROC_THREAD_ATTRIBUTE_CONSOLE = 0x00020016;

inline NTSTATUS openDevice(HANDLE& h, const wchar_t* name, ACCESS_MASK access, HANDLE root, ULONG options) {
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &n, OBJ_CASE_INSENSITIVE, root, nullptr);
    IO_STATUS_BLOCK io;
    return NtOpenFile(&h, access, &oa, &io, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, options);
}

class Device {
public:
    HANDLE server = nullptr, reference = nullptr;

    bool open(HANDLE inputAvailable) {
        if (openDevice(server, L"\\Device\\ConDrv\\Server", GENERIC_ALL, nullptr, 0) < 0) return false;
        if (openDevice(reference, L"\\Reference", GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, server, 0x20) < 0) return false;
        return call(SET_SERVER_INFORMATION, &inputAvailable, sizeof(inputAvailable), nullptr, 0);
    }

    bool read(const Completion* reply, Message& m) { return call(READ_IO, reply, reply ? sizeof(*reply) : 0, &m.d, sizeof(Message)); }
    bool complete(const Completion& c) { return call(COMPLETE_IO, &c, sizeof(c), nullptr, 0); }

    bool input(const Message& m, ULONG offset, void* p, ULONG n) {
        if (!n) return true;
        Operation op = { m.d.id, { p, n, offset } };
        return call(READ_INPUT, &op, sizeof(op), nullptr, 0);
    }

    bool output(const Message& m, ULONG offset, const void* p, ULONG n) {
        if (!n) return true;
        Operation op = { m.d.id, { p, n, offset } };
        return call(WRITE_OUTPUT, &op, sizeof(op), nullptr, 0);
    }

private:
    bool call(DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize) {
        thread_local HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED ov = {};
        ov.hEvent = done;
        DWORD n = 0;
        if (DeviceIoControl(server, code, const_cast<void*>(in), inSize, out, outSize, &n, &ov)) return true;
        if (GetLastError() != ERROR_IO_PENDING) return false;
        return GetOverlappedResult(server, &ov, &n, TRUE) != FALSE;
    }
};

}
