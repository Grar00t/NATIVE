#include "agent_connectors.hpp"
#include "../retrieval/native/niyah_utf8.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace agents {
namespace fs = std::filesystem;
using std::runtime_error;

/* Status records are control data, independent of the size of a monitored artifact. */
static constexpr size_t record_limit = 4096;

static bool regular(const fs::path &path)
{
    std::error_code ec;
    auto status = fs::status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
        return false;
    if (ec) throw fs::filesystem_error("status", path, ec);
    if (!fs::is_regular_file(status)) throw runtime_error("expected regular file: " + path.u8string());
    return true;
}

static void append_byte(std::string &record, char c)
{
    if (record.size() == record_limit) throw runtime_error("status record exceeds 4096 bytes");
    record += c;
}

static std::string finish(std::string record)
{
    if (!record.empty() && record.back() == '\r') record.pop_back();
    return record;
}

static Value file_value(const fs::path &path, bool size_only)
{
    if (!regular(path)) return {};
    if (size_only) {
        std::error_code ec;
        auto bytes = fs::file_size(path, ec);
        if (ec == std::errc::no_such_file_or_directory) return {};
        if (ec) throw fs::filesystem_error("file size", path, ec);
        return std::to_string(bytes);
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (!regular(path)) return {};
        throw runtime_error("cannot open status file: " + path.u8string());
    }
    std::string record;
    char c;
    while (input.get(c)) {
        if (c == '\n') {
            if (input.peek() != std::char_traits<char>::eof())
                throw runtime_error("expected one status record: " + path.u8string());
            if (input.bad()) throw runtime_error("status read failed: " + path.u8string());
            return finish(record);
        }
        append_byte(record, c);
    }
    if (input.bad()) throw runtime_error("status read failed: " + path.u8string());
    return {};  // The writer has not committed its line yet.
}

/* A producer may keep the pipe open and silent beyond the task deadline. */
class StdinRecords {
    std::string partial_;
    Value latest_;
    bool eof_ = false;
public:
    Value poll()
    {
        if (eof_) return latest_;
        char buffer[record_limit];
        size_t n;
#ifdef _WIN32
        HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
        DWORD kind = GetFileType(handle), available = sizeof buffer, got = 0;
        if (kind == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) { eof_ = true; return latest_; }
                throw runtime_error("cannot inspect status pipe");
            }
            if (!available) return latest_;
        } else if (kind != FILE_TYPE_DISK) {
            throw runtime_error("stdin status requires a pipe or a regular file");
        }
        DWORD want = available < sizeof buffer ? available : (DWORD)sizeof buffer;
        if (!ReadFile(handle, buffer, want, &got, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) { eof_ = true; return latest_; }
            throw runtime_error("cannot read status pipe");
        }
        n = got;
#else
        if (isatty(STDIN_FILENO)) throw runtime_error("stdin status requires a redirected input");
        struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
        int ready = ::poll(&fd, 1, 0);
        if (ready == 0 || (ready < 0 && errno == EINTR)) return latest_;
        if (ready < 0 || (fd.revents & POLLNVAL)) throw runtime_error("cannot inspect status input");
        auto got = read(STDIN_FILENO, buffer, sizeof buffer);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) return latest_;
        if (got < 0) throw runtime_error("cannot read status input");
        n = static_cast<size_t>(got);
#endif
        if (!n) eof_ = true;
        for (size_t i = 0; i < n; ++i) {
            if (buffer[i] == '\n') {
                latest_ = finish(partial_);
                partial_.clear();
            } else append_byte(partial_, buffer[i]);
        }
        return latest_;
    }
};

Reader open_input(const std::string &address)
{
    if (address == "stdin") {
        auto records = std::make_shared<StdinRecords>();
        return [records] { return records->poll(); };
    }
    auto colon = address.find(':');
    auto type = address.substr(0, colon);
    if ((type != "file" && type != "size") || colon == std::string::npos || colon+1 == address.size())
        throw runtime_error("input must be stdin, file:PATH or size:PATH");
    auto path = fs::u8path(address.substr(colon+1));
    return [path, by_size = type == "size"] { return file_value(path, by_size); };
}

Progress::Progress(const std::string &address) : stream_(&std::cout)
{
    if (address == "stdout") return;
    if (address == "stderr") { stream_ = &std::cerr; return; }
    if (address.compare(0, 5, "file:") != 0 || address.size() == 5)
        throw runtime_error("progress must be stdout, stderr or file:PATH");
    auto path = fs::u8path(address.substr(5));
    if (fs::exists(path) && !fs::is_regular_file(path))
        throw runtime_error("progress path must be a regular file");
    file_.open(path, std::ios::binary | std::ios::app);
    if (!file_) throw runtime_error("cannot open progress file: " + path.u8string());
    stream_ = &file_;
}

static void quoted(std::ostream &out, const std::string &s)
{
    static const char hex[] = "0123456789abcdef";
    auto bytes = reinterpret_cast<const unsigned char *>(s.data());
    out.put('"');
    for (size_t i = 0; i < s.size();) {
        size_t first = i;
        auto cp = niyah_utf8_next(bytes, s.size(), &i);
        if (cp == '"' || cp == '\\') { out.put('\\'); out.put(static_cast<char>(cp)); }
        else if (cp < 0x20) out << "\\u00" << hex[cp >> 4] << hex[cp & 15];
        else if (cp == 0xfffd && i == first+1) out << "\\ufffd";
        else out.write(s.data()+first, static_cast<std::streamsize>(i-first));
    }
    out.put('"');
}

void Progress::write(const std::string &task, const std::string &agent,
                     const char *event, const Value &value)
{
    auto &out = *stream_;
    out << "{\"task\":"; quoted(out, task);
    if (!agent.empty()) { out << ",\"agent\":"; quoted(out, agent); }
    out << ",\"event\":"; quoted(out, event);
    if (value) { out << ",\"value\":"; quoted(out, *value); }
    out << "}\n" << std::flush;
    if (!out) throw runtime_error("progress write failed");
}
}
