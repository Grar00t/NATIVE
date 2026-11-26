#include "agent_connectors.hpp"
#include "think.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
using agents::Value;
using std::runtime_error;
using Fields = std::map<std::string, std::string>;

const char usage[] =
    "think run --root DIR (--agent NAME | --trigger EVENT) --task ID\n"
    "          --timeout-ms N --poll-ms N [--bind NAME=SOURCE]... [--progress SINK]\n"
    "SOURCE: file:PATH | size:PATH | stdin     SINK: stdout (default) | stderr | file:PATH\n";

std::string trim(const std::string &s)
{
    auto begin = s.find_first_not_of(" \t\r");
    return begin == s.npos ? "" : s.substr(begin, s.find_last_not_of(" \t\r")-begin+1);
}

void check_name(const std::string &s)
{
    const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (s.empty() || !alnum(s.front()) || !std::all_of(s.begin(), s.end(), [&](char c) {
            return alnum(c) || c == '-' || c == '_';
        })) throw runtime_error("invalid identifier: " + s);
}

uint64_t number(const std::string &s)
{
    uint64_t v = 0;
    auto end = s.data() + s.size();
    auto result = std::from_chars(s.data(), end, v);
    if (result.ec != std::errc() || result.ptr != end) throw runtime_error("expected unsigned decimal: " + s);
    return v;
}

uint32_t duration(const std::string &s)
{
    uint64_t v = number(s);
    if (!v || v >= UINT32_MAX) throw runtime_error("duration must be between 1 and 4294967294 ms");
    return static_cast<uint32_t>(v);
}

const std::string &required(const Fields &fields, const std::string &key)
{
    auto item = fields.find(key);
    if (item == fields.end() || item->second.empty()) throw runtime_error("missing " + key);
    return item->second;
}

struct Options {
    fs::path root;
    std::string task, agent, trigger, progress;
    uint32_t timeout_ms, poll_ms;
    Fields bindings;
};

Options options(int argc, char **argv)
{
    Options o{};
    Fields p;
    const std::set<std::string> keys = {"--root","--task","--agent","--trigger","--progress","--timeout-ms","--poll-ms"};
    for (int i = 1; i < argc;) {
        std::string k = argv[i++];
        if (i == argc) throw runtime_error("missing value for " + k);
        std::string v = argv[i++];
        if (k == "--bind") {
            auto eq = v.find('=');
            if (eq == v.npos || eq+1 == v.size()) throw runtime_error("expected NAME=SOURCE");
            auto name = v.substr(0, eq);
            check_name(name);
            if (!o.bindings.emplace(name, v.substr(eq+1)).second) throw runtime_error("duplicate binding: " + name);
        } else if (!keys.count(k) || v.empty() || !p.emplace(k,v).second) {
            throw runtime_error("unknown, empty or duplicate option: " + k);
        }
    }
    o.root = fs::u8path(required(p, "--root"));
    o.task = required(p, "--task");
    o.timeout_ms = duration(required(p, "--timeout-ms"));
    o.poll_ms = duration(required(p, "--poll-ms"));
    o.progress = p.count("--progress") ? p.at("--progress") : "stdout";
    o.agent = p["--agent"];
    o.trigger = p["--trigger"];
    if (o.agent.empty() == o.trigger.empty()) throw runtime_error("choose one of --agent and --trigger");
    check_name(o.agent.empty() ? o.trigger : o.agent);
    if (o.progress.compare(0,5,"file:") == 0) {
        auto log = fs::u8path(o.progress.substr(5));
        for (const auto &binding : o.bindings) {
            const auto &uri = binding.second;
            if (uri.compare(0,5,"file:") && uri.compare(0,5,"size:")) continue;
            auto src = fs::u8path(uri.substr(5));
            if (log.empty() || src.empty()) throw runtime_error("empty file path");
            if (fs::weakly_canonical(fs::absolute(log)) == fs::weakly_canonical(fs::absolute(src)) ||
                (fs::exists(log) && fs::exists(src) && fs::equivalent(log,src)))
                throw runtime_error("progress and input refer to the same file");
        }
    }
    return o;
}

struct Manifest {
    std::string name, trigger, input, op, value;
    uint64_t minimum = 0;
    Value fail_value;
    std::vector<std::string> dependencies;
};

std::optional<Manifest> read_manifest(const fs::path &root, const std::string &name)
{
    auto file = root / fs::u8path(name) / "agent.ini";
    if (!fs::exists(file)) return {};
    if (!fs::is_regular_file(file)) throw runtime_error("expected regular manifest: " + file.u8string());
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        if (!fs::exists(file)) return {};
        throw runtime_error("cannot open " + file.u8string());
    }
    // Bound config reads even when a writer publishes an incorrect file.
    std::array<char,16384+1> bytes;
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || input.gcount() == static_cast<std::streamsize>(bytes.size()))
        throw runtime_error("unreadable or oversized manifest: " + name);
    std::istringstream lines(std::string(bytes.data(), static_cast<size_t>(input.gcount())));
    static const std::set<std::string> keys = {"version","name","trigger","input","op","value","fail","requires"};
    Fields fields;
    for (std::string line; std::getline(lines,line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        auto key = trim(line.substr(0,eq));
        if (eq == line.npos || !keys.count(key) || !fields.emplace(key,trim(line.substr(eq+1))).second)
            throw runtime_error("invalid or duplicate manifest field in " + name + ": " + key);
    }
    Manifest m;
    m.name = required(fields,"name");
    check_name(m.name);
    if (m.name != name || required(fields,"version") != "1") throw runtime_error("manifest name/version mismatch: " + name);
    m.trigger = required(fields,"trigger"); check_name(m.trigger);
    m.input = required(fields,"input"); check_name(m.input);
    m.op = required(fields,"op");
    m.value = required(fields,"value");
    if (m.op == "at-least") m.minimum = number(m.value);
    else if (m.op != "equals") throw runtime_error("unknown predicate: " + m.op);
    if (fields.count("fail")) m.fail_value = required(fields,"fail");
    std::istringstream deps(fields["requires"]);
    for (std::string dep; deps >> dep;) { check_name(dep); m.dependencies.push_back(dep); }
    return m;
}

class Run;
struct Pass {
    Run &run;
    std::map<std::string,std::optional<Manifest>> manifests;
    std::map<std::string,bool> finished;
    std::set<std::string> visiting;
    std::map<std::string,Value> observations;

    explicit Pass(Run &owner) : run(owner) {}
    const std::optional<Manifest> &manifest(const std::string &name);
    bool evaluate(const std::string &name);
};

class Run {
public:
    const Options &opts;
    agents::Progress progress;
    std::map<std::string,agents::Reader> readers;
    std::map<std::string,std::pair<std::string,Value>> previous;

    explicit Run(const Options &o) : opts(o), progress(o.progress)
    {
        if (!fs::is_directory(opts.root)) throw runtime_error("agent root must be an existing directory");
        for (const auto &binding : opts.bindings) {
            if (!readers.count(binding.second)) readers.emplace(binding.second, agents::open_input(binding.second));
        }
    }
    void changed(const std::string &agent, const std::string &event, const Value &value = {})
    {
        auto next = std::make_pair(event, value);
        auto old = previous.find(agent);
        if (old != previous.end() && old->second == next) return;
        progress.write(opts.task, agent, event.c_str(), value);
        previous[agent] = std::move(next);
    }
    bool poll()
    {
        Pass pass(*this);
        if (!opts.agent.empty()) return pass.evaluate(opts.agent);
        std::set<std::string> matches;
        for (const auto &entry : fs::directory_iterator(opts.root)) {
            if (!entry.is_directory()) continue;
            auto name = entry.path().filename().u8string();
            const auto &m = pass.manifest(name);
            if (m && m->trigger == opts.trigger) matches.insert(name);
        }
        if (matches.empty()) { changed("", "waiting_trigger", opts.trigger); return false; }
        bool all = true;
        for (const auto &name : matches) if (!pass.evaluate(name)) all = false;
        return all;
    }
};

const std::optional<Manifest> &Pass::manifest(const std::string &name)
{
    auto it = manifests.find(name);
    if (it == manifests.end()) it = manifests.emplace(name,read_manifest(run.opts.root,name)).first;
    return it->second;
}

bool Pass::evaluate(const std::string &name)
{
    auto known = finished.find(name);
    if (known != finished.end()) return known->second;
    if (!visiting.insert(name).second) throw runtime_error("dependency cycle at " + name);
    // Cap call-stack consumption from recursively nested manifests.
    if (visiting.size() > 128) throw runtime_error("dependency depth exceeds 128 at " + name);
    const auto &m = manifest(name);
    bool ready = false;
    if (!m) {
        run.changed(name,"waiting_manifest");
    } else {
        bool deps = true;
        for (const auto &dep : m->dependencies) if (!evaluate(dep)) deps = false;
        if (!deps) {
            run.changed(name,"waiting_dependencies");
        } else {
            const auto &address = required(run.opts.bindings,m->input);
            auto it = observations.find(address);
            if (it == observations.end()) it = observations.emplace(address,run.readers.at(address)()).first;
            const auto &v = it->second;
            if (v && m->fail_value == v) {
                run.changed(name,"rejected",v);
                throw runtime_error("failure value from " + name);
            }
            if (v) ready = m->op == "equals" ? *v == m->value : number(*v) >= m->minimum;
            run.changed(name,ready ? "ready" : "waiting_input",v);
        }
    }
    visiting.erase(name);
    finished.emplace(name,ready);
    return ready;
}

/* Catch exceptions here so they never cross the C connector ABI. */
ThinkState poll_agent(void *context, const ThinkTask *)
{
    try { return static_cast<Run *>(context)->poll() ? THINK_READY : THINK_WAIT; }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return THINK_FAILED; }
}

int report_agent(void *context, const ThinkTask *task, const char *event)
{
    try { static_cast<Run *>(context)->progress.write(task->id,"",event); return 0; }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return -1; }
}
}

extern "C" int agent_cli(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") { std::cout << usage; return THINK_OK; }
    Options o;
    try { o = options(argc,argv); }
    catch (const std::exception &e) { std::cerr << e.what() << '\n' << usage; return THINK_USAGE; }
    try {
        Run run(o);
        ThinkTask task{o.task.c_str(),o.timeout_ms,o.poll_ms};
        ThinkConnector connector{&run,poll_agent,report_agent};
        return think_run(&task,&connector);
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return THINK_ERROR; }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide)
{
    std::vector<std::string> utf8;
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,nullptr,0,nullptr,nullptr);
        if (!n) return THINK_USAGE;
        std::string s(static_cast<size_t>(n),'\0');
        if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,s.data(),n,nullptr,nullptr)) return THINK_USAGE;
        s.pop_back();
        utf8.push_back(std::move(s));
    }
    std::vector<char *> args;
    for (auto &s : utf8) args.push_back(s.data());
    args.push_back(nullptr);
    return think_cli(argc,args.data());
}
#endif
