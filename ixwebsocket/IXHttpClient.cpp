/*
 *  IXHttpClient.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2019 Machine Zone, Inc. All rights reserved.
 */

#include "IXHttpClient.h"
#include "IXUrlParser.h"
#include "IXWebSocketHttpHeaders.h"
#include "IXSocketFactory.h"

#include <sstream>
#include <iomanip>
#include <vector>
#include <cstring>

#include <zlib.h>

namespace ix
{
    const std::string HttpClient::kPost = "POST";
    const std::string HttpClient::kGet = "GET";
    const std::string HttpClient::kHead = "HEAD";
    const std::string HttpClient::kDel = "DEL";
    const std::string HttpClient::kPut = "PUT";

    HttpClient::HttpClient(bool async) : _async(async), _stop(false)
    {
        if (!_async) return;

        _thread = std::thread(&HttpClient::run, this);
    }

    HttpClient::~HttpClient()
    {
        if (!_thread.joinable()) return;

        _stop = true;
        _condition.notify_one();
        _thread.join();
    }

    HttpRequestArgsPtr HttpClient::createRequest(const std::string& url,
                                                 const std::string& verb)
    {
        auto request = std::make_shared<HttpRequestArgs>();
        request->url = url;
        request->verb = verb;
        return request;
    }

    bool HttpClient::performRequest(HttpRequestArgsPtr args,
                                    const OnResponseCallback& onResponseCallback)
    {
        if (!_async) return false;

        // Enqueue the task
        {
            // acquire lock
            std::unique_lock<std::mutex> lock(_queueMutex);

            // add the task
            _queue.push(std::make_pair(args, onResponseCallback));
        } // release lock

        // wake up one thread
        _condition.notify_one();

        return true;
    }

    void HttpClient::run()
    {
        while (true)
        {
            HttpRequestArgsPtr args;
            OnResponseCallback onResponseCallback;

            {
                std::unique_lock<std::mutex> lock(_queueMutex);

                while (!_stop && _queue.empty())
                {
                    _condition.wait(lock);
                }

                if (_stop) return;

                auto p = _queue.front();
                _queue.pop();

                args = p.first;
                onResponseCallback = p.second;
            }

            if (_stop) return;

            HttpResponsePtr response = request(args->url, args->verb, args->body, args);
            onResponseCallback(response);

            if (_stop) return;
        }
    }

    HttpResponsePtr HttpClient::request(
        const std::string& url,
        const std::string& verb,
        const std::string& body,
        HttpRequestArgsPtr args,
        int redirects)
    {
        // We only have one socket connection, so we cannot
        // make multiple requests concurrently.
        std::lock_guard<std::mutex> lock(_mutex);

        uint64_t uploadSize = 0;
        uint64_t downloadSize = 0;
        int code = 0;
        WebSocketHttpHeaders headers;
        std::string payload;

        std::string protocol, host, path, query;
        int port;

        if (!UrlParser::parse(url, protocol, host, path, query, port))
        {
            std::stringstream ss;
            ss << "Cannot parse url: " << url;
            return std::make_shared<HttpResponse>(code, HttpErrorCode::UrlMalformed,
                                headers, payload, ss.str(),
                                uploadSize, downloadSize);
        }

        bool tls = protocol == "https";
        std::string errorMsg;
        _socket = createSocket(tls, errorMsg);

        if (!_socket)
        {
            return std::make_shared<HttpResponse>(code, HttpErrorCode::CannotCreateSocket,
                                headers, payload, errorMsg,
                                uploadSize, downloadSize);
        }

        // Build request string
        std::stringstream ss;
        ss << verb << " " << path << " HTTP/1.1\r\n";
        ss << "Host: " << host << "\r\n";

        if (args->compress)
        {
            ss << "Accept-Encoding: gzip" << "\r\n";
        }

        // Append extra headers
        for (auto&& it : args->extraHeaders)
        {
            ss << it.first << ": " << it.second << "\r\n";
        }

        // Set a default Accept header if none is present
        if (headers.find("Accept") == headers.end())
        {
            ss << "Accept: */*" << "\r\n";
        }

        // Set a default User agent if none is present
        if (headers.find("User-Agent") == headers.end())
        {
            ss << "User-Agent: ixwebsocket" << "\r\n";
        }

        if (verb == kPost || verb == kPut)
        {
            ss << "Content-Length: " << body.size() << "\r\n";

            // Set default Content-Type if unspecified
            if (args->extraHeaders.find("Content-Type") == args->extraHeaders.end())
            {
                ss << "Content-Type: application/x-www-form-urlencoded" << "\r\n";
            }
            ss << "\r\n";
            ss << body;
        }
        else
        {
            ss << "\r\n";
        }

        std::string req(ss.str());
        std::string errMsg;
        std::atomic<bool> requestInitCancellation(false);

        // Make a cancellation object dealing with connection timeout
        auto isCancellationRequested =
            makeCancellationRequestWithTimeout(args->connectTimeout, requestInitCancellation);

        bool success = _socket->connect(host, port, errMsg, isCancellationRequested);
        if (!success)
        {
            std::stringstream ss;
            ss << "Cannot connect to url: " << url << " / error : " << errMsg;
            return std::make_shared<HttpResponse>(code, HttpErrorCode::CannotConnect,
                                                  headers, payload, ss.str(),
                                                  uploadSize, downloadSize);
        }

        // Make a new cancellation object dealing with transfer timeout
        isCancellationRequested =
            makeCancellationRequestWithTimeout(args->transferTimeout, requestInitCancellation);

        if (args->verbose)
        {
            std::stringstream ss;
            ss << "Sending " << verb << " request "
               << "to " << host << ":" << port << std::endl
               << "request size: " << req.size() << " bytes" << std::endl
               << "=============" << std::endl
               << req
               << "=============" << std::endl
               << std::endl;

            log(ss.str(), args);
        }

        if (!_socket->writeBytes(req, isCancellationRequested))
        {
            std::string errorMsg("Cannot send request");
            return std::make_shared<HttpResponse>(code, HttpErrorCode::SendError,
                                                  headers, payload, errorMsg,
                                                  uploadSize, downloadSize);
        }

        uploadSize = req.size();

        auto lineResult = _socket->readLine(isCancellationRequested);
        auto lineValid = lineResult.first;
        auto line = lineResult.second;

        if (!lineValid)
        {
            std::string errorMsg("Cannot retrieve status line");
            return std::make_shared<HttpResponse>(code, HttpErrorCode::CannotReadStatusLine,
                                                  headers, payload, errorMsg,
                                                  uploadSize, downloadSize);
        }

        if (args->verbose)
        {
            std::stringstream ss;
            ss << "Status line " << line;
            log(ss.str(), args);
        }

        if (sscanf(line.c_str(), "HTTP/1.1 %d", &code) != 1)
        {
            std::string errorMsg("Cannot parse response code from status line");
            return std::make_shared<HttpResponse>(code, HttpErrorCode::MissingStatus,
                                                  headers, payload, errorMsg,
                                                  uploadSize, downloadSize);
        }

        auto result = parseHttpHeaders(_socket, isCancellationRequested);
        auto headersValid = result.first;
        headers = result.second;

        if (!headersValid)
        {
            std::string errorMsg("Cannot parse http headers");
            return std::make_shared<HttpResponse>(code, HttpErrorCode::HeaderParsingError,
                                                  headers, payload, errorMsg,
                                                  uploadSize, downloadSize);
        }

        // Redirect ?
        if ((code >= 301 && code <= 308) && args->followRedirects)
        {
            if (headers.find("Location") == headers.end())
            {
                std::string errorMsg("Missing location header for redirect");
                return std::make_shared<HttpResponse>(code, HttpErrorCode::MissingLocation,
                                                      headers, payload, errorMsg,
                                                      uploadSize, downloadSize);
            }

            if (redirects >= args->maxRedirects)
            {
                std::stringstream ss;
                ss << "Too many redirects: " << redirects;
                return std::make_shared<HttpResponse>(code, HttpErrorCode::TooManyRedirects,
                                                      headers, payload, ss.str(),
                                                      uploadSize, downloadSize);
            }

            // Recurse
            std::string location = headers["Location"];
            return request(location, verb, body, args, redirects+1);
        }

        if (verb == "HEAD")
        {
            return std::make_shared<HttpResponse>(code, HttpErrorCode::Ok,
                                                  headers, payload, std::string(),
                                                  uploadSize, downloadSize);
        }

        // Parse response:
        if (headers.find("Content-Length") != headers.end())
        {
            ssize_t contentLength = -1;
            ss.str("");
            ss << headers["Content-Length"];
            ss >> contentLength;

            payload.reserve(contentLength);

            auto chunkResult = _socket->readBytes(contentLength,
                                                  args->onProgressCallback,
                                                  isCancellationRequested);
            if (!chunkResult.first)
            {
                errorMsg = "Cannot read chunk";
                return std::make_shared<HttpResponse>(code, HttpErrorCode::ChunkReadError,
                                                      headers, payload, errorMsg,
                                                      uploadSize, downloadSize);
            }
            payload += chunkResult.second;
        }
        else if (headers.find("Transfer-Encoding") != headers.end() &&
                 headers["Transfer-Encoding"] == "chunked")
        {
            std::stringstream ss;

            while (true)
            {
                lineResult = _socket->readLine(isCancellationRequested);
                line = lineResult.second;

                if (!lineResult.first)
                {
                    return std::make_shared<HttpResponse>(code, HttpErrorCode::ChunkReadError,
                                                          headers, payload, errorMsg,
                                                          uploadSize, downloadSize);
                }

                uint64_t chunkSize;
                ss.str("");
                ss << std::hex << line;
                ss >> chunkSize;

                if (args->verbose)
                {
                    std::stringstream oss;
                    oss << "Reading " << chunkSize << " bytes"
                        << std::endl;
                    log(oss.str(), args);
                }

                payload.reserve(payload.size() + (size_t) chunkSize);

                // Read a chunk
                auto chunkResult = _socket->readBytes((size_t) chunkSize,
                                                      args->onProgressCallback,
                                                      isCancellationRequested);
                if (!chunkResult.first)
                {
                    errorMsg = "Cannot read chunk";
                    return std::make_shared<HttpResponse>(code, HttpErrorCode::ChunkReadError,
                                                          headers, payload, errorMsg,
                                                          uploadSize, downloadSize);
                }
                payload += chunkResult.second;

                // Read the line that terminates the chunk (\r\n)
                lineResult = _socket->readLine(isCancellationRequested);

                if (!lineResult.first)
                {
                    return std::make_shared<HttpResponse>(code, HttpErrorCode::ChunkReadError,
                                                          headers, payload, errorMsg,
                                                          uploadSize, downloadSize);
                }

                if (chunkSize == 0) break;
            }
        }
        else if (code == 204)
        {
            ; // 204 is NoContent response code
        }
        else
        {
            std::string errorMsg("Cannot read http body");
            return std::make_shared<HttpResponse>(code, HttpErrorCode::CannotReadBody,
                                                  headers, payload, errorMsg,
                                                  uploadSize, downloadSize);
        }

        downloadSize = payload.size();

        // If the content was compressed with gzip, decode it
        if (headers["Content-Encoding"] == "gzip")
        {
            std::string decompressedPayload;
            if (!gzipInflate(payload, decompressedPayload))
            {
                std::string errorMsg("Error decompressing payload");
                return std::make_shared<HttpResponse>(code, HttpErrorCode::Gzip,
                                                      headers, payload, errorMsg,
                                                      uploadSize, downloadSize);
            }
            payload = decompressedPayload;
        }

        return std::make_shared<HttpResponse>(code, HttpErrorCode::Ok,
                                              headers, payload, std::string(),
                                              uploadSize, downloadSize);
    }

    HttpResponsePtr HttpClient::get(const std::string& url,
                                    HttpRequestArgsPtr args)
    {
        return request(url, kGet, std::string(), args);
    }

    HttpResponsePtr HttpClient::head(const std::string& url,
                                     HttpRequestArgsPtr args)
    {
        return request(url, kHead, std::string(), args);
    }

    HttpResponsePtr HttpClient::del(const std::string& url,
                                    HttpRequestArgsPtr args)
    {
        return request(url, kDel, std::string(), args);
    }

    HttpResponsePtr HttpClient::post(const std::string& url,
                                     const HttpParameters& httpParameters,
                                     HttpRequestArgsPtr args)
    {
        return request(url, kPost, serializeHttpParameters(httpParameters), args);
    }

    HttpResponsePtr HttpClient::post(const std::string& url,
                                     const std::string& body,
                                     HttpRequestArgsPtr args)
    {
        return request(url, kPost, body, args);
    }

    HttpResponsePtr HttpClient::put(const std::string& url,
                                    const HttpParameters& httpParameters,
                                    HttpRequestArgsPtr args)
    {
        return request(url, kPut, serializeHttpParameters(httpParameters), args);
    }

    HttpResponsePtr HttpClient::put(const std::string& url,
                                    const std::string& body,
                                    const HttpRequestArgsPtr args)
    {
        return request(url, kPut, body, args);
    }

    std::string HttpClient::urlEncode(const std::string& value)
    {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (std::string::const_iterator i = value.begin(), n = value.end();
             i != n; ++i)
        {
            std::string::value_type c = (*i);

            // Keep alphanumeric and other accepted characters intact
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            {
                escaped << c;
                continue;
            }

            // Any other characters are percent-encoded
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int((unsigned char) c);
            escaped << std::nouppercase;
        }

        return escaped.str();
    }

    std::string HttpClient::serializeHttpParameters(const HttpParameters& httpParameters)
    {
        std::stringstream ss;
        size_t count = httpParameters.size();
        size_t i = 0;

        for (auto&& it : httpParameters)
        {
            ss << urlEncode(it.first)
               << "="
               << urlEncode(it.second);

            if (i++ < (count-1))
            {
               ss << "&";
            }
        }
        return ss.str();
    }

    bool HttpClient::gzipInflate(
        const std::string& in,
        std::string& out)
    {
        z_stream inflateState;
        std::memset(&inflateState, 0, sizeof(inflateState));

        inflateState.zalloc = Z_NULL;
        inflateState.zfree = Z_NULL;
        inflateState.opaque = Z_NULL;
        inflateState.avail_in = 0;
        inflateState.next_in = Z_NULL;

        if (inflateInit2(&inflateState, 16+MAX_WBITS) != Z_OK)
        {
            return false;
        }

        inflateState.avail_in = (uInt) in.size();
        inflateState.next_in = (unsigned char *)(const_cast<char *>(in.data()));

        const int kBufferSize = 1 << 14;

        std::unique_ptr<unsigned char[]> compressBuffer =
            std::make_unique<unsigned char[]>(kBufferSize);

        do
        {
            inflateState.avail_out = (uInt) kBufferSize;
            inflateState.next_out = compressBuffer.get();

            int ret = inflate(&inflateState, Z_SYNC_FLUSH);

            if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR)
            {
                inflateEnd(&inflateState);
                return false;
            }

            out.append(
                reinterpret_cast<char *>(compressBuffer.get()),
                kBufferSize - inflateState.avail_out
            );
        } while (inflateState.avail_out == 0);

        inflateEnd(&inflateState);
        return true;
    }

    void HttpClient::log(const std::string& msg,
                         HttpRequestArgsPtr args)
    {
        if (args->logger)
        {
            args->logger(msg);
        }
    }
}
