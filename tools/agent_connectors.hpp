#ifndef NATIVE_AGENT_CONNECTORS_HPP
#define NATIVE_AGENT_CONNECTORS_HPP

#include <fstream>
#include <functional>
#include <optional>
#include <string>

namespace agents {
using Value = std::optional<std::string>;
using Reader = std::function<Value()>;
Reader open_input(const std::string &address);

class Progress {
    std::ofstream file_;
    std::ostream *stream_;
public:
    explicit Progress(const std::string &address);
    void write(const std::string &task, const std::string &agent,
               const char *event, const Value &value = {});
};
}
#endif
