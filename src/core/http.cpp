#include "xrkit/core/http.h"

#include "xrkit/core/theme.h"

#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace xrkit::core {

bool network_enabled() {
    return load_user_value("network", "enabled", "1") != "0";
}

namespace {

#ifdef _WIN32

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

std::string from_wide(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

struct WinHttpHandles {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    ~WinHttpHandles() {
        if (request != nullptr) WinHttpCloseHandle(request);
        if (connect != nullptr) WinHttpCloseHandle(connect);
        if (session != nullptr) WinHttpCloseHandle(session);
    }
};

HttpResult winhttp_get(const std::string& url, int timeout_sec, std::size_t max_bytes,
                       std::ofstream* file, const std::wstring& extra_headers) {
    HttpResult result;
    const std::wstring wurl = to_wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512]{};
    wchar_t path[4096]{};
    wchar_t extra[4096]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        result.error = "URL 解析失败";
        return result;
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && uc.nScheme != INTERNET_SCHEME_HTTP) {
        result.error = "只允许 http/https";
        return result;
    }

    WinHttpHandles handles;
    handles.session = WinHttpOpen(L"xrkit/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (handles.session == nullptr) {
        handles.session = WinHttpOpen(L"xrkit/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (handles.session == nullptr) {
        result.error = "WinHttpOpen 失败";
        return result;
    }
    const int timeout_ms = (timeout_sec > 0 ? timeout_sec : 15) * 1000;
    WinHttpSetTimeouts(handles.session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(handles.session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    const INTERNET_PORT port = uc.nPort != 0 ? uc.nPort
                                             : (secure ? INTERNET_DEFAULT_HTTPS_PORT
                                                       : INTERNET_DEFAULT_HTTP_PORT);
    handles.connect = WinHttpConnect(handles.session, uc.lpszHostName, port, 0);
    if (handles.connect == nullptr) {
        result.error = "WinHttpConnect 失败";
        return result;
    }
    std::wstring target = path;
    if (target.empty()) target = L"/";
    if (extra[0] != L'\0') target += extra;
    handles.request = WinHttpOpenRequest(handles.connect, L"GET", target.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0);
    if (handles.request == nullptr) {
        result.error = "WinHttpOpenRequest 失败";
        return result;
    }
    const std::wstring headers =
        L"User-Agent: xrkit/2.0\r\nAccept: application/vnd.github+json\r\n" + extra_headers;
    if (!WinHttpSendRequest(handles.request, headers.c_str(),
                            static_cast<DWORD>(-1L), nullptr, 0, 0, 0)) {
        result.error = "WinHttpSendRequest 失败";
        return result;
    }
    if (!WinHttpReceiveResponse(handles.request, nullptr)) {
        result.error = "WinHttpReceiveResponse 失败";
        return result;
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(handles.request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    result.status = static_cast<int>(status);
    result.ok = status >= 200 && status < 400;

    std::vector<char> buffer(64 * 1024);
    while (true) {
        DWORD read = 0;
        if (!WinHttpReadData(handles.request, buffer.data(),
                             static_cast<DWORD>(buffer.size()), &read)) {
            result.error = "WinHttpReadData 失败";
            result.ok = false;
            break;
        }
        if (read == 0) break;
        if (result.bytes + read > max_bytes) {
            const std::size_t room = max_bytes - result.bytes;
            if (file != nullptr) file->write(buffer.data(), static_cast<std::streamsize>(room));
            else result.body.append(buffer.data(), room);
            result.bytes += room;
            result.truncated = true;
            break;
        }
        if (file != nullptr) file->write(buffer.data(), static_cast<std::streamsize>(read));
        else result.body.append(buffer.data(), read);
        result.bytes += read;
    }
    return result;
}

#endif

}  // namespace

HttpResult http_get(const std::string& url, int timeout_sec, std::size_t max_bytes) {
    if (!network_enabled()) {
        HttpResult result;
        result.error = "联网功能已关闭";
        return result;
    }
#ifdef _WIN32
    return winhttp_get(url, timeout_sec, max_bytes, nullptr, L"");
#else
    HttpResult result;
    result.error = "当前构建没有 WinHTTP";
    return result;
#endif
}

HttpResult http_download(const std::string& url, const std::string& path, int timeout_sec,
                         std::size_t max_bytes) {
    if (!network_enabled()) {
        HttpResult result;
        result.error = "联网功能已关闭";
        return result;
    }
    // 断点续传：半成品写 <path>.part；失败保留，下次 Range 续传；成功才改名到 path。
    const std::string part = path + ".part";
    std::error_code ec;
    std::size_t existing = 0;
    if (std::filesystem::is_regular_file(part, ec)) {
        existing = static_cast<std::size_t>(std::filesystem::file_size(part, ec));
        if (ec) existing = 0;
    }
    const auto request = [&](bool resume, HttpResult& result) {
        std::ofstream file(part, std::ios::binary |
                                     (resume ? std::ios::app : std::ios::trunc));
        if (!file) {
            result.error = "无法创建下载临时文件";
            result.ok = false;
            return;
        }
        std::wstring extra;
        if (resume) extra = L"Range: bytes=" + std::to_wstring(existing) + L"-\r\n";
        result = winhttp_get(url, timeout_sec, max_bytes, &file, extra);
        file.close();
    };

    HttpResult result;
    request(existing > 0, result);
    // 服务器忽略 Range 返回 200：不能把完整内容追加到半文件后面，重下一次。
    if (existing > 0 && result.status == 200) {
        std::filesystem::remove(part, ec);
        existing = 0;
        request(false, result);
    }
    if (!result.ok || result.truncated) {
        // 保留 .part：被断网/超时打断后下次可续传。
        if (result.truncated) result.error = "下载超过大小上限";
        result.ok = false;
        return result;
    }
    std::filesystem::remove(path, ec);
    std::filesystem::rename(part, path, ec);
    if (ec) {
        result.ok = false;
        result.error = "下载完成但无法落到目标文件：" + ec.message();
    }
    return result;
}

}  // namespace xrkit::core
