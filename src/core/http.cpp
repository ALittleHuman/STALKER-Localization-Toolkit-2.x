// xrkit::core —— WinHTTP 底层原语（GET/POST/下载/Range）。
// 联网总开关、代理/直连回退、超时与大小上限都在这里；上层不要绕过它。

#include "xrkit/core/http.h"

#include "xrkit/core/theme.h"

#include <cctype>
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

// 回环地址永远不经过代理：IE/WinINET 的代理经常没有把 127.0.0.1 列进 bypass，
// WinHTTP 会把本地请求老老实实发给代理 —— 代理要么 404、要么拖到超时
// （实测本机代理 127.0.0.1:26561：一次 /health 白等 24 秒）。入口统一短路。
bool is_loopback_url(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    const std::size_t begin = scheme + 3;
    std::size_t end = url.find_first_of("/?#", begin);
    if (end == std::string::npos) end = url.size();
    std::string host = url.substr(begin, end - begin);
    const std::size_t at = host.rfind('@');
    if (at != std::string::npos) host.erase(0, at + 1);
    if (!host.empty() && host.front() == '[') {
        const std::size_t close = host.find(']');
        host = host.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    } else {
        const std::size_t colon = host.find(':');
        if (colon != std::string::npos) host.erase(colon);
    }
    std::string lower;
    lower.reserve(host.size());
    for (const char c : host) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "localhost" || lower == "::1") return true;
    return lower.rfind("127.", 0) == 0;
}

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

// 静态系统代理（Internet 选项）默认不会被 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 采用；
// 这里显式读取当前用户的 IE/WinINET 代理并交给 WinHTTP，否则“浏览器/gh 能上网、工具检测更新超时”。
void apply_ie_proxy(HINTERNET session) {
    if (session == nullptr) return;
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&config)) return;
    if (config.lpszProxy != nullptr && config.lpszProxy[0] != L'\0') {
        WINHTTP_PROXY_INFO info{};
        info.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        info.lpszProxy = config.lpszProxy;
        info.lpszProxyBypass = config.lpszProxyBypass;
        WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &info, sizeof(info));
    }
    if (config.lpszProxy != nullptr) GlobalFree(config.lpszProxy);
    if (config.lpszProxyBypass != nullptr) GlobalFree(config.lpszProxyBypass);
    if (config.lpszAutoConfigUrl != nullptr) GlobalFree(config.lpszAutoConfigUrl);
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

HttpResult winhttp_request_once(const std::string& method, const std::string& url,
                                const std::string& body, const std::string& content_type,
                                int timeout_sec, std::size_t max_bytes, std::ofstream* file,
                                const std::wstring& extra_headers, DWORD access_type,
                                const HttpProgress& progress = {}) {
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
    handles.session = WinHttpOpen(L"xrkit/2.0", access_type,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (handles.session == nullptr && access_type != WINHTTP_ACCESS_TYPE_NO_PROXY) {
        handles.session = WinHttpOpen(L"xrkit/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (handles.session == nullptr) {
        result.error = "WinHttpOpen 失败";
        return result;
    }
    if (access_type == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY) apply_ie_proxy(handles.session);
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
    const std::wstring wmethod = to_wide(method);
    handles.request = WinHttpOpenRequest(handles.connect, wmethod.c_str(), target.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0);
    if (handles.request == nullptr) {
        result.error = "WinHttpOpenRequest 失败";
        return result;
    }
    std::wstring headers =
        L"User-Agent: xrkit/2.0\r\nAccept: application/vnd.github+json\r\n" + extra_headers;
    if (!content_type.empty()) headers += L"Content-Type: " + to_wide(content_type) + L"\r\n";
    if (!body.empty()) headers += L"Content-Length: " + std::to_wstring(body.size()) + L"\r\n";
    void* body_ptr = body.empty() ? nullptr : const_cast<char*>(body.data());
    const DWORD body_len = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(handles.request, headers.c_str(),
                            static_cast<DWORD>(-1L), body_ptr, body_len, 0, 0)) {
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

    // ★ 2026-11-19：记下"应该收多少字节"。200 看 Content-Length；
    //   206（续传）看 Content-Range 的总长（`bytes a-b/total`）。
    //   没有这个头 ⇒ expected_bytes=0，调用方不做完整性判断。
    if (status == 206) {
        wchar_t range[128] = {};
        DWORD range_size = sizeof(range);
        if (WinHttpQueryHeaders(handles.request, WINHTTP_QUERY_CONTENT_RANGE,
                                WINHTTP_HEADER_NAME_BY_INDEX, range, &range_size,
                                WINHTTP_NO_HEADER_INDEX)) {
            const std::wstring text(range);
            const std::size_t slash = text.rfind(L'/');
            if (slash != std::wstring::npos) {
                try {
                    result.expected_bytes = static_cast<std::size_t>(std::stoull(text.substr(slash + 1)));
                } catch (...) {
                    result.expected_bytes = 0;
                }
            }
        }
    } else {
        DWORD length = 0;
        DWORD length_size = sizeof(length);
        if (WinHttpQueryHeaders(handles.request,
                                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size,
                                WINHTTP_NO_HEADER_INDEX)) {
            result.expected_bytes = static_cast<std::size_t>(length);
        }
    }

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
        if (progress && !progress(result.bytes, result.expected_bytes)) {
            result.ok = false;
            result.error = "下载已取消";
            break;
        }
    }
    return result;
}

#endif

}  // namespace

HttpResult winhttp_request(const std::string& method, const std::string& url,
                           const std::string& body, const std::string& content_type,
                           int timeout_sec, std::size_t max_bytes, std::ofstream* file,
                           const std::wstring& extra_headers,
                           const HttpProgress& progress = {}) {
    HttpResult result = winhttp_request_once(method, url, body, content_type, timeout_sec,
                                             max_bytes, file, extra_headers,
                                             WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, progress);
    // 静态代理/加速器可能只对浏览器有效；代理路径失败时换直连再试一次。
    // 文件下载不重试（第一次可能已写入部分字节，重试会重复追加）。
    if (!result.ok && file == nullptr) {
        HttpResult direct = winhttp_request_once(method, url, body, content_type, timeout_sec,
                                                 max_bytes, file, extra_headers,
                                                 WINHTTP_ACCESS_TYPE_NO_PROXY, progress);
        if (direct.ok || direct.status != 0) return direct;
    }
    return result;
}

HttpResult http_get(const std::string& url, int timeout_sec, std::size_t max_bytes,
                    bool prefer_direct, bool allow_disabled) {
    if (!allow_disabled && !network_enabled()) {
        HttpResult result;
        result.error = "联网功能已关闭";
        return result;
    }
#ifdef _WIN32
    if (prefer_direct || is_loopback_url(url)) {
        return winhttp_request_once("GET", url, std::string(), std::string(), timeout_sec,
                                    max_bytes, nullptr, L"", WINHTTP_ACCESS_TYPE_NO_PROXY);
    }
    return winhttp_request("GET", url, std::string(), std::string(), timeout_sec, max_bytes,
                           nullptr, L"");
#else
    HttpResult result;
    result.error = "当前构建没有 WinHTTP";
    return result;
#endif
}

HttpResult http_download(const std::string& url, const std::string& path, int timeout_sec,
                         std::size_t max_bytes, const HttpProgress& progress,
                         bool prefer_direct, bool allow_disabled) {
    if (!allow_disabled && !network_enabled()) {
        HttpResult result;
        result.error = "联网功能已关闭";
        return result;
    }
    const bool direct = prefer_direct || is_loopback_url(url);
    // 断点续传：半成品写 <path>.part；失败保留，下次 Range 续传；成功才改名到 path。
    const std::string part = path + ".part";
    std::error_code ec;
    std::size_t existing = 0;
    if (std::filesystem::is_regular_file(part, ec)) {
        existing = static_cast<std::size_t>(std::filesystem::file_size(part, ec));
        if (ec) existing = 0;
    }
    const auto request = [&](bool resume, bool use_direct, HttpResult& result) {
        std::ofstream file(part, std::ios::binary |
                                     (resume ? std::ios::app : std::ios::trunc));
        if (!file) {
            result.error = "无法创建下载临时文件";
            result.ok = false;
            return;
        }
        std::wstring extra;
        if (resume) extra = L"Range: bytes=" + std::to_wstring(existing) + L"-\r\n";
        const HttpProgress report = [&](std::size_t bytes, std::size_t total) {
            if (!progress) return true;
            return progress((resume ? existing : 0) + bytes, total);
        };
        if (use_direct) {
            result = winhttp_request_once("GET", url, std::string(), std::string(), timeout_sec,
                                          max_bytes, &file, extra,
                                          WINHTTP_ACCESS_TYPE_NO_PROXY, report);
        } else {
            result = winhttp_request("GET", url, std::string(), std::string(), timeout_sec,
                                     max_bytes, &file, extra, report);
        }
        file.close();
        // ★ 2026-11-19：服务端提前关门（FIN）时 WinHttpReadData 返回 0，循环按"读完了"退出，
        //   `ok` 仍是 true ⇒ 半成品会被 rename 成成品。这里按响应声明的总长兜底：
        //   收到的不够 ⇒ 判失败、**保留 .part**，下次 Range 续传。
        if (result.ok && !result.truncated && result.expected_bytes > 0) {
            const std::size_t have = (resume ? existing : 0) + result.bytes;
            if (have < result.expected_bytes) {
                result.ok = false;
                result.error = "下载不完整（" + std::to_string(have) + "/" +
                               std::to_string(result.expected_bytes) + "）";
            }
        }
    };

    HttpResult result;
    request(existing > 0, direct, result);
    // ★ 2026-11-23（BETA.256）：代理路径完全没拿到响应（status==0 且 0 字节）⇒ 丢掉
    //   半文件，直连再试一次。只认这一种：4xx/5xx 是服务端明确答复，换路由重打没意义。
    if (!result.ok && !direct && result.status == 0 && result.bytes == 0) {
        std::filesystem::remove(part, ec);
        existing = 0;
        request(false, true, result);
    }
    // 半成品比远端还大（换源 / 远端重打包）⇒ Range 越界（416）：丢掉 .part 重来，
    // 否则每次重试都 416，永远下不完。
    if (existing > 0 && result.status == 416) {
        std::filesystem::remove(part, ec);
        existing = 0;
        request(false, direct, result);
    }
    // 服务器忽略 Range 返回 200：不能把完整内容追加到半文件后面，重下一次。
    if (existing > 0 && result.status == 200) {
        std::filesystem::remove(part, ec);
        existing = 0;
        request(false, direct, result);
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

std::uint64_t http_probe_size(const std::string& url, int timeout_sec,
                              bool prefer_direct, bool allow_disabled) {
    if (!allow_disabled && !network_enabled()) return 0;
    const std::wstring range = L"Range: bytes=0-0\r\n";
#ifdef _WIN32
    HttpResult result;
    if (prefer_direct || is_loopback_url(url)) {
        result = winhttp_request_once("GET", url, std::string(), std::string(), timeout_sec,
                                      1, nullptr, range, WINHTTP_ACCESS_TYPE_NO_PROXY);
    } else {
        result = winhttp_request("GET", url, std::string(), std::string(), timeout_sec, 1,
                                 nullptr, range);
    }
    if (result.status == 206 || result.status == 200) return result.expected_bytes;
#endif
    return 0;
}

HttpResult http_download_range(const std::string& url, const std::string& path,
                               std::uint64_t start, std::uint64_t end,
                               int timeout_sec, const HttpProgress& progress,
                               bool prefer_direct, bool allow_disabled) {
    HttpResult result;
    if (end < start) {
        result.error = "非法 Range";
        return result;
    }
    if (!allow_disabled && !network_enabled()) {
        result.error = "联网功能已关闭";
        return result;
    }
    const bool direct = prefer_direct || is_loopback_url(url);
    const std::uint64_t want = end - start + 1;
    std::error_code ec;
    std::uint64_t existing = 0;
    if (std::filesystem::is_regular_file(path, ec)) {
        existing = static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
        if (ec) existing = 0;
    }
    if (existing > want) {
        std::filesystem::remove(path, ec);
        existing = 0;
    }
    if (existing == want) {
        result.ok = true;
        result.status = 206;
        result.bytes = static_cast<std::size_t>(want);
        result.expected_bytes = static_cast<std::size_t>(want);
        if (progress) progress(result.bytes, result.expected_bytes);
        return result;
    }
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) {
        result.error = "无法创建分片文件";
        return result;
    }
    const std::uint64_t from = start + existing;
    std::wstring extra = L"Range: bytes=" + std::to_wstring(from) + L"-" +
                         std::to_wstring(end) + L"\r\n";
    const HttpProgress report = [&](std::size_t bytes, std::size_t) {
        if (progress) {
            return progress(static_cast<std::size_t>(existing) + bytes,
                            static_cast<std::size_t>(want));
        }
        return true;
    };
#ifdef _WIN32
    if (direct) {
        result = winhttp_request_once("GET", url, std::string(), std::string(), timeout_sec,
                                      static_cast<std::size_t>(want), &file, extra,
                                      WINHTTP_ACCESS_TYPE_NO_PROXY, report);
    } else {
        result = winhttp_request("GET", url, std::string(), std::string(), timeout_sec,
                                 static_cast<std::size_t>(want), &file, extra, report);
    }
#else
    result.error = "当前构建没有 WinHTTP";
#endif
    file.close();
    // ★ 2026-11-23（BETA.256）：代理路径 0 响应 0 字节 ⇒ 清分片，直连重取这一段。
    if (!result.ok && !direct && result.status == 0 && result.bytes == 0) {
        std::filesystem::remove(path, ec);
        return http_download_range(url, path, start, end, timeout_sec, progress, true,
                                   allow_disabled);
    }
    if (result.status == 200) {
        // 服务器忽略 Range：不能把整包追加到分片里。
        result.ok = false;
        result.error = "服务器不支持 Range";
        std::filesystem::remove(path, ec);
        return result;
    }
    if (result.status == 416) {
        result.ok = false;
        result.error = "分片 Range 越界";
        std::filesystem::remove(path, ec);
        return result;
    }
    if (!result.ok) return result;
    const std::uint64_t have = existing + static_cast<std::uint64_t>(result.bytes);
    if (have < want) {
        result.ok = false;
        result.error = "分片下载不完整";
        return result;
    }
    result.ok = true;
    result.bytes = static_cast<std::size_t>(want);
    result.expected_bytes = static_cast<std::size_t>(want);
    return result;
}

HttpResult http_post(const std::string& url, const std::string& body,
                     const std::string& content_type, int timeout_sec,
                     std::size_t max_bytes, bool allow_disabled) {
    if (!allow_disabled && !network_enabled()) {
        HttpResult result;
        result.error = "联网功能已关闭";
        return result;
    }
#ifdef _WIN32
    if (is_loopback_url(url)) {
        return winhttp_request_once("POST", url, body,
                                    content_type.empty() ? "application/json" : content_type,
                                    timeout_sec, max_bytes, nullptr, L"",
                                    WINHTTP_ACCESS_TYPE_NO_PROXY);
    }
    return winhttp_request("POST", url, body,
                           content_type.empty() ? "application/json" : content_type,
                           timeout_sec, max_bytes, nullptr, L"");
#else
    HttpResult result;
    result.error = "当前构建没有 WinHTTP";
    return result;
#endif
}

}  // namespace xrkit::core
