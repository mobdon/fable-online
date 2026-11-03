// Blocking HTTP(S) over WinHTTP. Call from a worker thread, never the UI thread.
#pragma once
#include <functional>
#include <string>

namespace http {

struct Response {
    int status = 0;            // 0 = no response (network error, see error)
    std::string body;
    std::string error;
};

// True for http:// URLs only when the host is this machine (dev gateway).
bool url_allowed(const std::string& url);

Response request(const std::string& method, const std::string& url,
                 const std::string& body = "", const std::string& bearer = "");

// Streams url into path. progress(done, total) may get total = 0 if unknown.
// Returning false from progress cancels.
Response download(const std::string& url, const std::wstring& path,
                  const std::function<bool(unsigned long long, unsigned long long)>& progress);

}  // namespace http
