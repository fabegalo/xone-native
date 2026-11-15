// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace xboxone::diagnostics {
// Bounded view of real session output. Clearing the view never changes the log.
class LogBuffer {
public:
    static constexpr std::size_t byte_limit=2*1024*1024, line_limit=10000, fragment_limit=16384;
    void clear() { lines.clear(); fragment.clear(); bytes=0; omitted=0; }
    void append(std::string_view input) {
        for(char c:input) {
            if(c=='\n') { if(!fragment.empty() && fragment.back()=='\r')fragment.pop_back(); push(); }
            else { fragment+=c; if(fragment.size()>=fragment_limit)push(); }
        }
    }
    std::deque<std::string> lines;
    std::string fragment;
    std::size_t omitted=0;
private:
    void push() {
        bytes+=fragment.size(); lines.push_back(std::move(fragment)); fragment.clear();
        while(bytes>byte_limit || lines.size()>line_limit) {
            bytes-=lines.front().size();lines.pop_front();++omitted;
        }
    }
    std::size_t bytes=0;
};

#ifdef _WIN32
class Console {
public:
    Console();
    ~Console();
    Console(const Console&)=delete;
    Console& operator=(const Console&)=delete;
    bool show(const std::filesystem::path& source,std::uintptr_t owner=0);
    void hide();
private:
    friend int run_console(const std::filesystem::path& source);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int run_console(const std::filesystem::path& source);
#endif
}
