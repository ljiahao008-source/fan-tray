#include "pawnio.h"

#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kDeviceType = 41394u << 16;
constexpr uint32_t kFnNameLength = 32;
constexpr uint32_t kIoctlLoadBinary = kDeviceType | (0x821u << 2);
constexpr uint32_t kIoctlExecute = kDeviceType | (0x841u << 2);

constexpr wchar_t kDevicePath[] = L"\\\\?\\GLOBALROOT\\Device\\PawnIO";

// 执行驱动函数：name 为 32 字节内的 ASCII 函数名，input 为 long[] 参数。
// 返回输出 long[] 数量（读回字节数 / 8）。失败返回 -1。
int ExecuteFn(HANDLE h, const char* name, const int64_t* input, size_t inputCount,
              int64_t* output, size_t outputCapacity) {
    std::vector<BYTE> inBuf(kFnNameLength + inputCount * sizeof(int64_t), 0);
    std::memcpy(inBuf.data(), name, std::min<size_t>(kFnNameLength - 1, std::strlen(name)));
    if (inputCount > 0)
        std::memcpy(inBuf.data() + kFnNameLength, input, inputCount * sizeof(int64_t));

    std::vector<BYTE> outBuf(outputCapacity * sizeof(int64_t), 0);
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(h, kIoctlExecute, inBuf.data(), (DWORD)inBuf.size(),
                         outBuf.data(), (DWORD)outBuf.size(), &bytesReturned, nullptr))
        return -1;

    size_t count = bytesReturned / sizeof(int64_t);
    if (count > 0 && count <= outputCapacity)
        std::memcpy(output, outBuf.data(), count * sizeof(int64_t));
    return (int)count;
}

}  // namespace

bool PawnIo::LoadModuleFromResource(HINSTANCE hInst, WORD resId) {
    if (_h != INVALID_HANDLE_VALUE)
        return true;

    if (hInst == nullptr)
        hInst = GetModuleHandleW(nullptr);

    HANDLE h = CreateFileW(kDevicePath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    HRSRC res = FindResourceW(hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!res) {
        CloseHandle(h);
        return false;
    }

    HGLOBAL hg = LoadResource(hInst, res);
    void* data = (hg != nullptr) ? LockResource(hg) : nullptr;
    DWORD len = (hg != nullptr) ? SizeofResource(hInst, res) : 0;
    if (!data || len == 0) {
        CloseHandle(h);
        return false;
    }

    BOOL ok = DeviceIoControl(h, kIoctlLoadBinary, data, len, nullptr, 0, nullptr, nullptr);
    if (!ok) {
        CloseHandle(h);
        return false;
    }

    _h = h;
    return true;
}

bool PawnIo::ReadMsr(uint32_t index, uint32_t& eax, uint32_t& edx) {
    if (_h == INVALID_HANDLE_VALUE)
        return false;

    int64_t input[1] = {index};
    int64_t output[1] = {0};
    if (ExecuteFn(_h, "ioctl_read_msr", input, 1, output, 1) != 1)
        return false;

    uint64_t v = (uint64_t)output[0];
    eax = (uint32_t)(v & 0xFFFFFFFFu);
    edx = (uint32_t)(v >> 32);
    return true;
}

void PawnIo::Close() {
    if (_h != INVALID_HANDLE_VALUE) {
        CloseHandle(_h);
        _h = INVALID_HANDLE_VALUE;
    }
}
