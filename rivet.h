#ifndef NJLANE314_RIVET_H_INCLUDED
#define NJLANE314_RIVET_H_INCLUDED

// rivet.h - a small, content-aware dependency runner for C++17.
//
// Rivetfile syntax:
//
//   all <- report.pdf
//
//   result.json <- input.csv @local
//       ./analyse $< --output $@
//
//   publish! <- report.pdf @local
//       ./publish $<
//
// A rule produces exactly one regular file. A recipe-less rule is an alias.
// A target ending in '!' is an always-run action. $@, $<, and $^ mean the
// transactional output, first input, and all file inputs respectively.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rivet {

inline constexpr std::string_view version = "0.1.0";

class error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {

inline std::string trim(std::string value) {
    const auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

inline bool starts_indented(std::string_view value) {
    return !value.empty() && (value.front() == ' ' || value.front() == '\t');
}

inline bool ends_with(std::string_view value, char suffix) {
    return !value.empty() && value.back() == suffix;
}

inline std::string normalize_reference(std::string value) {
    const std::filesystem::path path(value);
    for (const auto& component : path) {
        if (component == "..")
            return value; // lexical collapse across a symlink would change meaning
    }
    return path.lexically_normal().generic_string();
}

inline std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::string word;
    char quote = 0;
    bool escaped = false;
    bool started = false;
    for (char c : text) {
        if (escaped) {
            word.push_back(c);
            escaped = false;
            started = true;
            continue;
        }
        if (c == '\\' && quote != '\'') {
            escaped = true;
            started = true;
            continue;
        }
        if (quote) {
            if (c == quote)
                quote = 0;
            else
                word.push_back(c);
            started = true;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            started = true;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (started) {
                out.push_back(std::move(word));
                word.clear();
                started = false;
            }
            continue;
        }
        word.push_back(c);
        started = true;
    }
    if (escaped)
        throw error("Trailing escape in rule header");
    if (quote)
        throw error("Unterminated quote in rule header");
    if (started)
        out.push_back(std::move(word));
    return out;
}

inline std::string shell_quote(std::string_view value) {
    if (value.empty())
        return "''";
    std::string out = "'";
    for (char c : value) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

inline void replace_all(std::string& text, std::string_view needle, std::string_view replacement) {
    if (needle.empty())
        return;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        text.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

inline std::string expand_shell(std::string_view recipe, std::string_view output,
                                std::string_view first, std::string_view all) {
    std::string result;
    char quote = 0;
    bool escaped = false;
    for (std::size_t i = 0; i < recipe.size(); ++i) {
        const char c = recipe[i];
        if (escaped) {
            result.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\' && quote != '\'') {
            result.push_back(c);
            escaped = true;
            continue;
        }
        if (c == '\'' || c == '"') {
            if (!quote)
                quote = c;
            else if (quote == c)
                quote = 0;
            result.push_back(c);
            continue;
        }
        if (c != '$' || i + 1 >= recipe.size()) {
            result.push_back(c);
            continue;
        }
        const char automatic = recipe[i + 1];
        if (automatic == '$') {
            result.push_back('$');
            ++i;
            continue;
        }
        if (automatic != '@' && automatic != '<' && automatic != '^') {
            result.push_back(c);
            continue;
        }
        if (quote)
            throw error("Automatic variables in @shell recipes must be unquoted");
        if (automatic == '@')
            result.append(output);
        else if (automatic == '<')
            result.append(first);
        else
            result.append(all);
        ++i;
    }
    return result;
}

// Compact SHA-256 implementation used for source, recipe, and output identities.
class sha256 {
public:
    sha256() { reset(); }

    void update(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        total_bytes_ += static_cast<std::uint64_t>(size);
        while (size) {
            const std::size_t take = std::min(size, buffer_.size() - buffered_);
            std::copy_n(bytes, take, buffer_.data() + buffered_);
            buffered_ += take;
            bytes += take;
            size -= take;
            if (buffered_ == buffer_.size()) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    void update(std::string_view text) { update(text.data(), text.size()); }

    std::string finish() {
        const std::uint64_t bits = static_cast<std::uint64_t>(total_bytes_) * 8u;
        buffer_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), 0);
            transform(buffer_.data());
            buffered_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.begin() + 56, 0);
        for (int i = 0; i != 8; ++i)
            buffer_[56 + static_cast<std::size_t>(i)] =
                static_cast<unsigned char>(bits >> (56 - 8 * i));
        transform(buffer_.data());

        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (std::uint32_t value : state_)
            out << std::setw(8) << value;
        const std::string result = out.str();
        reset();
        return result;
    }

private:
    static constexpr std::array<std::uint32_t, 64> constants_ = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    static std::uint32_t rotate(std::uint32_t x, unsigned n) {
        return (x >> n) | (x << (32u - n));
    }

    static std::uint32_t read32(const unsigned char* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24) |
               (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) |
               static_cast<std::uint32_t>(p[3]);
    }

    void transform(const unsigned char* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i != 16; ++i)
            words[i] = read32(block + 4 * i);
        for (std::size_t i = 16; i != words.size(); ++i) {
            const std::uint32_t s0 = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^
                                     (words[i - 15] >> 3);
            const std::uint32_t s1 = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^
                                     (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        for (std::size_t i = 0; i != words.size(); ++i) {
            const std::uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + s1 + choose + constants_[i] + words[i];
            const std::uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    void reset() {
        state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        buffer_.fill(0);
        buffered_ = 0;
        total_bytes_ = 0;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

inline std::string hash_text(std::string_view text) {
    sha256 digest;
    digest.update(text);
    return digest.finish();
}

inline std::string hash_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw error("Cannot open input: " + path.string());
    sha256 digest;
    std::array<char, 64 * 1024> buffer{};
    while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount())
        digest.update(buffer.data(), static_cast<std::size_t>(input.gcount()));
    if (input.bad())
        throw error("Cannot read input: " + path.string());
    return digest.finish();
}

inline bool valid_digest(std::string_view value) {
    if (value.size() != 64)
        return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

class fingerprint {
public:
    void add(std::string_view value) {
        const std::string length = std::to_string(value.size());
        digest_.update(length);
        digest_.update(":");
        digest_.update(value);
        digest_.update(";");
    }
    std::string finish() { return digest_.finish(); }

private:
    sha256 digest_;
};

inline std::string unique_suffix() {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return ".tmp." + std::to_string(now);
}

inline void ensure_parent(const std::filesystem::path& path) {
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
}

inline void replace_file(const std::filesystem::path& source, const std::filesystem::path& target) {
    ensure_parent(target);
    std::error_code ec;
    std::filesystem::rename(source, target, ec);
    if (ec)
        throw error("Cannot publish " + target.string() + ": " + ec.message());
}

inline void copy_atomically(const std::filesystem::path& source,
                            const std::filesystem::path& target) {
    ensure_parent(target);
    const auto temporary = target.string() + unique_suffix();
    std::error_code ec;
    std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        throw error("Cannot copy artifact to " + target.string() + ": " + ec.message());
    replace_file(temporary, target);
}

class workspace_lock {
public:
    workspace_lock(const std::filesystem::path& path, bool enabled) {
#if defined(__unix__) || defined(__APPLE__)
        if (!enabled)
            return;
        ensure_parent(path);
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
        if (descriptor_ < 0)
            throw error("Cannot open workspace lock: " + path.string());
        while (::flock(descriptor_, LOCK_EX) != 0) {
            if (errno != EINTR) {
                ::close(descriptor_);
                descriptor_ = -1;
                throw error("Cannot acquire workspace lock: " + path.string());
            }
        }
#else
        (void)path;
        (void)enabled;
#endif
    }

    workspace_lock(const workspace_lock&) = delete;
    workspace_lock& operator=(const workspace_lock&) = delete;

    ~workspace_lock() {
#if defined(__unix__) || defined(__APPLE__)
        if (descriptor_ >= 0) {
            (void)::flock(descriptor_, LOCK_UN);
            (void)::close(descriptor_);
        }
#endif
    }

private:
#if defined(__unix__) || defined(__APPLE__)
    int descriptor_ = -1;
#endif
};

inline std::map<std::string, std::string> read_record(const std::filesystem::path& path) {
    std::map<std::string, std::string> result;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const auto equals = line.find('=');
        if (equals != std::string::npos)
            result.emplace(line.substr(0, equals), line.substr(equals + 1));
    }
    return result;
}

inline void write_record(const std::filesystem::path& path,
                         const std::vector<std::pair<std::string, std::string>>& fields) {
    ensure_parent(path);
    const auto temporary = path.string() + unique_suffix();
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw error("Cannot write receipt: " + path.string());
        for (const auto& field : fields) {
            if (field.first.find_first_of("=\r\n") != std::string::npos ||
                field.second.find_first_of("\r\n") != std::string::npos)
                throw error("Receipt fields must be line-oriented");
            output << field.first << '=' << field.second << '\n';
        }
        if (!output)
            throw error("Cannot finish receipt: " + path.string());
    }
    replace_file(temporary, path);
}

} // namespace detail

struct artifact_input {
    std::string logical_name;
    std::filesystem::path local_path;
    std::string digest;
    bool available = true;
    bool provisional = false;
};

struct invocation {
    std::string target;
    std::string task_key;
    std::string raw_recipe;
    std::vector<std::string> recipe;
    std::vector<std::string> argv;
    std::string command;
    std::vector<artifact_input> inputs;
    std::filesystem::path output;
    std::filesystem::path workspace;
    std::filesystem::path project_root;
};

struct plan {
    std::map<std::string, std::string> facts;
    std::vector<std::string> warnings;
};

struct result {
    int exit_code = 0;
    std::map<std::string, std::string> metadata;
    bool success() const noexcept { return exit_code == 0; }
};

using log_sink = std::function<void(std::string_view)>;

struct executor {
    std::string identity;
    std::function<rivet::plan(const invocation&)> plan;
    std::function<rivet::result(const invocation&, log_sink)> run;
    bool shell = false;
};

struct run_options {
    std::filesystem::path file = "Rivetfile";
    std::vector<std::string> targets;
    bool dry_run = false;
    bool force = false;
    bool why = false;
};

inline executor local_executor(std::string identity = "local-argv-v1") {
    executor out;
    out.identity = std::move(identity);
    out.plan = [](const invocation&) {
        rivet::plan value;
        value.facts.emplace("executor", "local");
        return value;
    };
    out.run = [](const invocation& call, log_sink) {
        result value;
#if defined(__unix__) || defined(__APPLE__)
        if (call.argv.empty()) {
            value.exit_code = 127;
            return value;
        }
        std::vector<char*> arguments;
        arguments.reserve(call.argv.size() + 1);
        for (const auto& argument : call.argv)
            arguments.push_back(const_cast<char*>(argument.c_str()));
        arguments.push_back(nullptr);
        const pid_t pid = ::fork();
        if (pid < 0) {
            value.exit_code = 127;
            return value;
        }
        if (pid == 0) {
            if (::chdir(call.project_root.c_str()) != 0)
                ::_exit(126);
            ::execvp(arguments.front(), arguments.data());
            ::_exit(errno == ENOENT ? 127 : 126);
        }
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                value.exit_code = 127;
                return value;
            }
        }
        if (WIFEXITED(status)) {
            value.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            value.exit_code = 128 + WTERMSIG(status);
        } else {
            value.exit_code = 125;
        }
#else
        std::string command = "cd " + detail::shell_quote(call.project_root.string()) + " &&";
        for (const auto& argument : call.argv)
            command += " " + detail::shell_quote(argument);
        const int status = std::system(command.c_str());
        value.exit_code = status == 0 ? 0 : 1;
#endif
        return value;
    };
    return out;
}

inline executor shell_executor(std::string identity = "shell-v1") {
    executor out;
    out.identity = std::move(identity);
    out.shell = true;
    out.plan = [](const invocation&) {
        rivet::plan value;
        value.facts.emplace("executor", "shell");
        return value;
    };
    out.run = [](const invocation& call, log_sink) {
        const std::string command = "cd " + detail::shell_quote(call.project_root.string()) +
                                    " && " + call.command;
        const int status = std::system(command.c_str());
        result value;
        if (status == -1) {
            value.exit_code = 127;
#if defined(__unix__) || defined(__APPLE__)
        } else if (WIFEXITED(status)) {
            value.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            value.exit_code = 128 + WTERMSIG(status);
#endif
        } else {
            value.exit_code = status == 0 ? 0 : 1;
        }
        return value;
    };
    return out;
}

class engine {
public:
    engine() {
        profile("local", local_executor("local-argv-v1"));
        profile("shell", shell_executor());
    }

    void profile(std::string name, executor implementation) {
        if (name.empty() || name.find_first_of(" \t\r\n@") != std::string::npos)
            throw error("Invalid executor profile name: " + name);
        if (implementation.identity.empty() || !implementation.run)
            throw error("Executor profile is incomplete: " + name);
        profiles_[std::move(name)] = std::move(implementation);
    }

    int cli(int argc, char** argv, std::filesystem::path default_file = "Rivetfile") {
        run_options selected;
        selected.file = std::move(default_file);
        for (int i = 1; i < argc; ++i) {
            const std::string argument(argv[i]);
            if (argument == "-f") {
                if (++i == argc)
                    throw error("-f requires a path");
                selected.file = argv[i];
            } else if (argument == "-n" || argument == "--dry-run") {
                selected.dry_run = true;
            } else if (argument == "-B" || argument == "--force") {
                selected.force = true;
            } else if (argument == "--why") {
                selected.why = true;
            } else if (argument == "--version") {
                std::cout << "rivet " << version << '\n';
                return 0;
            } else if (argument == "-h" || argument == "--help") {
                help(std::cout);
                return 0;
            } else if (!argument.empty() && argument.front() == '-') {
                throw error("Unknown option: " + argument);
            } else {
                selected.targets.push_back(argument);
            }
        }

        return run(std::move(selected));
    }

    int run(run_options selected = {}) {
        current_ = selected;
        load(selected.file);
        if (selected.targets.empty())
            selected.targets.push_back(order_.front());
        for (auto& target : selected.targets) {
            if (detail::ends_with(target, '!'))
                target.pop_back();
            target = detail::normalize_reference(std::move(target));
        }
        current_ = selected;
        detail::workspace_lock lock(state_root_ / "lock", !selected.dry_run);
        states_.clear();
        results_.clear();
        validate(selected.targets);
        bool failed = false;
        for (const std::string& target : selected.targets) {
            try {
                (void)build(target, {});
            } catch (const std::exception& failure) {
                std::cerr << "rivet: " << failure.what() << '\n';
                failed = true;
                break;
            }
        }
        return failed ? 1 : 0;
    }

    static void help(std::ostream& out) {
        out << "usage: rivet [-f FILE] [-n] [-B] [--why] [TARGET ...]\n"
               "  -n, --dry-run  print work without executing it\n"
               "  -B, --force    rebuild every artifact rule\n"
               "      --why      explain cache decisions\n";
    }

private:
    struct rule {
        std::string target;
        std::vector<std::string> dependencies;
        std::string profile = "local";
        std::string recipe;
        std::vector<std::string> recipe_tokens;
        bool action = false;
        std::size_t line = 0;
    };

    struct built {
        std::string digest;
        std::optional<std::filesystem::path> artifact;
        bool would_change = false;
        bool provisional = false;
    };

    enum class visit { unseen, active, done };

    void validate(const std::vector<std::string>& targets) const {
        std::map<std::string, visit, std::less<>> marks;
        for (std::string target : targets) {
            if (detail::ends_with(target, '!'))
                target.pop_back();
            validate_name(target, marks, {});
        }
    }

    void validate_name(const std::string& name,
                       std::map<std::string, visit, std::less<>>& marks,
                       std::vector<std::string> stack) const {
        const auto found = rules_.find(name);
        if (found == rules_.end()) {
            if (!std::filesystem::is_regular_file(resolve(name)))
                throw error("No rule to make target '" + name + "'");
            return;
        }
        if (marks[name] == visit::done)
            return;
        if (marks[name] == visit::active) {
            stack.push_back(name);
            std::ostringstream cycle;
            for (std::size_t i = 0; i < stack.size(); ++i) {
                if (i)
                    cycle << " -> ";
                cycle << stack[i];
            }
            throw error("Dependency cycle: " + cycle.str());
        }

        const rule& selected = found->second;
        if (!selected.recipe.empty() && !profiles_.count(selected.profile))
            throw error("Unknown executor profile '@" + selected.profile + "' for " + name);
        marks[name] = visit::active;
        stack.push_back(name);
        for (const auto& dependency : selected.dependencies) {
            const auto dependency_rule = rules_.find(dependency);
            if (dependency_rule != rules_.end() && dependency_rule->second.action)
                throw error("Action '" + dependency + "' may not be a dependency of '" + name + "'");
            validate_name(dependency, marks, stack);
        }
        marks[name] = visit::done;
    }

    void load(const std::filesystem::path& file) {
        rules_.clear();
        order_.clear();
        states_.clear();
        results_.clear();

        const auto absolute = std::filesystem::absolute(file);
        root_ = absolute.parent_path();
        state_root_ = root_ / ".rivet";
        std::ifstream input(absolute);
        if (!input)
            throw error("Cannot open " + absolute.string());

        rule* current = nullptr;
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const std::string cleaned = detail::trim(line);
            if (cleaned.empty() || cleaned.front() == '#')
                continue;
            if (detail::starts_indented(line)) {
                if (!current)
                    parse_error(line_number, "recipe without a rule");
                if (!current->recipe.empty())
                    parse_error(line_number, "only one recipe line is supported");
                current->recipe = cleaned;
                try {
                    current->recipe_tokens = detail::words(cleaned);
                } catch (const std::exception& failure) {
                    parse_error(line_number, failure.what());
                }
                if (current->recipe_tokens.empty())
                    parse_error(line_number, "empty recipe");
                continue;
            }

            const auto arrow = line.find("<-");
            if (arrow == std::string::npos)
                parse_error(line_number, "expected '<-'");
            const auto targets = detail::words(detail::trim(line.substr(0, arrow)));
            if (targets.size() != 1)
                parse_error(line_number, "a rule must have exactly one target");

            rule parsed;
            parsed.target = targets.front();
            parsed.line = line_number;
            if (detail::ends_with(parsed.target, '!')) {
                parsed.action = true;
                parsed.target.pop_back();
            }
            if (parsed.target.empty())
                parse_error(line_number, "empty target");
            const std::filesystem::path raw_target(parsed.target);
            for (const auto& component : raw_target)
                if (component == "..")
                    parse_error(line_number, "targets may not contain '..'");
            const std::filesystem::path target_path = raw_target.lexically_normal();
            if (target_path.is_absolute())
                parse_error(line_number, "targets must be relative paths");
            if (target_path.empty() || target_path == "." || !target_path.has_filename())
                parse_error(line_number, "target must name a file or action");
            if (!target_path.empty() && *target_path.begin() == ".rivet")
                parse_error(line_number, "targets may not use the reserved .rivet directory");
            if (target_path.generic_string() != parsed.target)
                parse_error(line_number, "target path is not normalized; use '" +
                                             target_path.generic_string() + "'");

            auto right = detail::words(line.substr(arrow + 2));
            if (!right.empty() && !right.back().empty() && right.back().front() == '@') {
                parsed.profile = right.back().substr(1);
                right.pop_back();
                if (parsed.profile.empty())
                    parse_error(line_number, "empty executor profile");
            }
            for (const auto& item : right) {
                if (!item.empty() && item.front() == '@')
                    parse_error(line_number, "executor profile must be the final token");
            }
            for (auto& item : right)
                item = detail::normalize_reference(std::move(item));
            parsed.dependencies = std::move(right);

            if (rules_.count(parsed.target))
                parse_error(line_number, "duplicate target: " + parsed.target);
            order_.push_back(parsed.target);
            auto inserted = rules_.emplace(parsed.target, std::move(parsed));
            current = &inserted.first->second;
        }
        if (order_.empty())
            throw error("Rivetfile contains no rules");
        for (const auto& item : rules_) {
            const rule& selected = item.second;
            if (selected.action && selected.recipe.empty())
                parse_error(item.second.line, "an action requires a recipe");
            bool uses_output = false;
            for (std::string token : selected.recipe_tokens) {
                detail::replace_all(token, "$$", "\x1f");
                uses_output = uses_output || token.find("$@") != std::string::npos;
                if (token.find("$^") != std::string::npos && token != "$^")
                    parse_error(selected.line, "$^ must occupy a complete recipe argument");
            }
            if (!selected.action && !selected.recipe.empty() && !uses_output)
                parse_error(selected.line, "an artifact recipe must write $@");
        }
    }

    [[noreturn]] void parse_error(std::size_t line, const std::string& message) const {
        throw error(current_.file.string() + ':' + std::to_string(line) + ": " + message);
    }

    built build(const std::string& name, std::vector<std::string> stack) {
        const auto known = results_.find(name);
        if (known != results_.end())
            return known->second;

        auto rule_it = rules_.find(name);
        if (rule_it == rules_.end()) {
            const auto path = resolve(name);
            if (!std::filesystem::is_regular_file(path))
                throw error("No rule to make target '" + name + "'");
            built source{detail::hash_file(path), path, false};
            results_[name] = source;
            return source;
        }

        const visit state = states_[name];
        if (state == visit::active) {
            stack.push_back(name);
            std::ostringstream cycle;
            for (std::size_t i = 0; i < stack.size(); ++i) {
                if (i)
                    cycle << " -> ";
                cycle << stack[i];
            }
            throw error("Dependency cycle: " + cycle.str());
        }
        if (state == visit::done)
            return results_.at(name);

        states_[name] = visit::active;
        stack.push_back(name);
        rule& selected = rule_it->second;
        std::vector<built> dependencies;
        dependencies.reserve(selected.dependencies.size());
        bool dependency_would_change = false;
        for (const auto& dependency : selected.dependencies) {
            built value = build(dependency, stack);
            dependency_would_change = dependency_would_change || value.would_change;
            dependencies.push_back(std::move(value));
        }

        built value;
        if (selected.recipe.empty()) {
            detail::fingerprint digest;
            digest.add("rivet-alias-v1");
            digest.add(selected.target);
            for (const auto& dependency : dependencies)
                digest.add(dependency.digest);
            value.digest = digest.finish();
            value.would_change = dependency_would_change;
        } else {
            value = execute(selected, dependencies, dependency_would_change);
        }

        states_[name] = visit::done;
        results_[name] = value;
        return value;
    }

    built execute(const rule& selected, const std::vector<built>& dependencies,
                  bool dependency_would_change) {
        const auto profile_it = profiles_.find(selected.profile);
        if (profile_it == profiles_.end())
            throw error("Unknown executor profile '@" + selected.profile + "' for " + selected.target);
        const executor& implementation = profile_it->second;

        std::vector<artifact_input> artifact_inputs;
        std::vector<std::string> input_names;
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            if (!dependencies[i].artifact)
                continue;
            artifact_inputs.push_back(
                {selected.dependencies[i],
                 dependencies[i].provisional ? std::filesystem::path{}
                                             : *dependencies[i].artifact,
                 dependencies[i].digest,
                 !dependencies[i].provisional,
                 dependencies[i].provisional});
            input_names.push_back(selected.dependencies[i]);
        }
        bool uses_first = false;
        for (std::string token : selected.recipe_tokens) {
            detail::replace_all(token, "$$", "\x1f");
            uses_first = uses_first || token.find("$<") != std::string::npos;
        }
        if (uses_first && artifact_inputs.empty())
            throw error("$< used by target without file inputs: " + selected.target);
        if (implementation.shell)
            (void)detail::expand_shell(selected.recipe, "OUTPUT", "FIRST", "ALL");

        detail::fingerprint identity;
        identity.add("rivet-task-v1");
        identity.add(selected.target);
        identity.add(selected.recipe);
        identity.add(selected.profile);
        identity.add(implementation.identity);
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            identity.add(selected.dependencies[i]);
            const auto& dependency = dependencies[i];
            identity.add(dependency.digest);
        }
        const std::string task_key = identity.finish();

        const auto receipt = receipt_path(task_key);
        const auto record = detail::read_record(receipt);
        const auto target = resolve(selected.target);
        std::string reason;
        bool cached = false;
        bool restored = false;

        if (selected.action) {
            reason = "action targets always run";
        } else if (current_.force) {
            reason = "forced by -B";
        } else if (dependency_would_change && current_.dry_run) {
            reason = "a dependency would change";
        } else if (record.empty()) {
            reason = "no receipt for this task identity";
        } else if (record_value(record, "VERSION") != "1" ||
                   record_value(record, "TARGET") != selected.target ||
                   record_value(record, "TASK_KEY") != task_key) {
            reason = "invalid receipt";
        } else {
            const std::string output_hash = record_value(record, "OUTPUT_HASH");
            if (!detail::valid_digest(output_hash)) {
                reason = "invalid output digest in receipt";
            } else {
                const auto object = object_path(output_hash);
                const bool valid_object = std::filesystem::is_regular_file(object) &&
                                          !std::filesystem::is_symlink(
                                              std::filesystem::symlink_status(object)) &&
                                          detail::hash_file(object) == output_hash;
                if (!valid_object) {
                    reason = "cached object is missing";
                } else {
                    const bool published = std::filesystem::is_regular_file(target) &&
                                           !std::filesystem::is_symlink(
                                               std::filesystem::symlink_status(target)) &&
                                           detail::hash_file(target) == output_hash;
                    if (!published && current_.dry_run) {
                        explain(selected.target, "would restore from cache");
                        std::cout << selected.target << " <- cache\n";
                        return {output_hash, object, false};
                    }
                    if (!published) {
                        detail::copy_atomically(object, target);
                        restored = true;
                    }
                    cached = true;
                }
            }
        }

        if (cached) {
            explain(selected.target, restored ? "restored from cache" : "current");
            return {record_value(record, "OUTPUT_HASH"), target, false};
        }

        explain(selected.target, reason);
        const auto workspace = state_root_ / "work" / task_key;
        const auto transactional_output = workspace / "output" /
                                          std::filesystem::path(selected.target).filename();
        const std::string logical_output = transactional_output.lexically_relative(root_).generic_string();
        const std::string output_word = detail::shell_quote(logical_output);
        const std::string first_word = input_names.empty() ? std::string{} :
                                       detail::shell_quote(input_names.front());
        std::string all_words;
        for (const auto& name : input_names) {
            if (!all_words.empty())
                all_words.push_back(' ');
            all_words += detail::shell_quote(name);
        }
        const std::string command = implementation.shell
                                        ? detail::expand_shell(selected.recipe, output_word,
                                                               first_word, all_words)
                                        : selected.recipe;

        std::vector<std::string> arguments;
        for (std::string token : selected.recipe_tokens) {
            if (token == "$^") {
                arguments.insert(arguments.end(), input_names.begin(), input_names.end());
                continue;
            }
            constexpr std::string_view sentinel = "\x1fRIVET_DOLLAR\x1f";
            detail::replace_all(token, "$$", sentinel);
            detail::replace_all(token, "$@", logical_output);
            detail::replace_all(token, "$<", input_names.empty() ? std::string{} :
                                                          input_names.front());
            detail::replace_all(token, sentinel, "$");
            arguments.push_back(std::move(token));
        }

        invocation call{selected.target, task_key, selected.recipe, selected.recipe_tokens,
                        std::move(arguments), command, std::move(artifact_inputs),
                        transactional_output, workspace, root_};

        if (current_.dry_run) {
            rivet::plan preview;
            const bool deferred = std::any_of(
                call.inputs.begin(), call.inputs.end(),
                [](const artifact_input& input) { return input.provisional; });
            if (implementation.plan && !deferred)
                preview = implementation.plan(call);
            if (deferred)
                preview.facts.emplace("plan", "deferred");
            std::cout << selected.target << " <- @" << selected.profile;
            for (const auto& fact : preview.facts)
                std::cout << ' ' << fact.first << '=' << fact.second;
            std::cout << '\n';
            for (const auto& warning : preview.warnings)
                std::cerr << selected.target << ": warning: " << warning << '\n';
            return {task_key, selected.action ? std::optional<std::filesystem::path>{}
                                             : std::optional<std::filesystem::path>{target},
                    true, !selected.action};
        }

        std::error_code cleanup_error;
        std::filesystem::remove_all(workspace, cleanup_error);
        if (cleanup_error)
            throw error("Cannot clean task workspace: " + cleanup_error.message());
        std::error_code create_error;
        std::filesystem::create_directories(transactional_output.parent_path(), create_error);
        if (create_error)
            throw error("Cannot create task workspace: " + create_error.message());

        const auto started = std::chrono::steady_clock::now();
        result outcome = implementation.run(call, [](std::string_view line) {
            std::cerr << line;
            if (line.empty() || line.back() != '\n')
                std::cerr << '\n';
        });
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        if (!outcome.success()) {
            std::filesystem::remove_all(workspace, cleanup_error);
            throw error("Target '" + selected.target + "' failed with exit code " +
                        std::to_string(outcome.exit_code));
        }

        if (selected.action) {
            std::filesystem::remove_all(workspace, cleanup_error);
            return {task_key, std::nullopt, true};
        }
        if (!std::filesystem::is_regular_file(
                std::filesystem::symlink_status(transactional_output))) {
            std::filesystem::remove_all(workspace, cleanup_error);
            throw error("Target '" + selected.target +
                        "' succeeded but did not create a regular, non-symlink $@");
        }

        const std::string output_hash = detail::hash_file(transactional_output);
        const auto object = object_path(output_hash);
        const bool object_valid = std::filesystem::is_regular_file(object) &&
                                  !std::filesystem::is_symlink(
                                      std::filesystem::symlink_status(object)) &&
                                  detail::hash_file(object) == output_hash;
        if (!object_valid) {
            std::error_code remove_error;
            std::filesystem::remove(object, remove_error);
            if (remove_error)
                throw error("Cannot replace corrupt cache object: " + remove_error.message());
            detail::copy_atomically(transactional_output, object);
        }
        detail::copy_atomically(object, target);
        detail::write_record(receipt,
                             {{"VERSION", "1"},
                              {"TARGET", selected.target},
                              {"TASK_KEY", task_key},
                              {"OUTPUT_HASH", output_hash},
                              {"PROFILE", selected.profile},
                              {"ELAPSED_MS", std::to_string(elapsed.count())}});
        std::filesystem::remove_all(workspace, cleanup_error);
        return {output_hash, target, true};
    }

    void explain(const std::string& target, const std::string& reason) const {
        if (current_.why)
            std::cout << target << ": " << reason << '\n';
    }

    std::filesystem::path resolve(const std::string& path) const {
        const std::filesystem::path value(path);
        return value.is_absolute() ? value : std::filesystem::absolute(root_ / value);
    }

    std::filesystem::path receipt_path(const std::string& task_key) const {
        if (!detail::valid_digest(task_key))
            throw error("Invalid task identity");
        return state_root_ / "receipts" / (task_key + ".txt");
    }

    std::filesystem::path object_path(const std::string& digest) const {
        if (!detail::valid_digest(digest))
            throw error("Invalid artifact digest");
        return state_root_ / "objects" / digest.substr(0, 2) / digest.substr(2);
    }

    static std::string record_value(const std::map<std::string, std::string>& record,
                                    const std::string& key) {
        const auto found = record.find(key);
        return found == record.end() ? std::string{} : found->second;
    }

    std::map<std::string, executor, std::less<>> profiles_;
    std::map<std::string, rule, std::less<>> rules_;
    std::vector<std::string> order_;
    std::map<std::string, visit, std::less<>> states_;
    std::map<std::string, built, std::less<>> results_;
    std::filesystem::path root_;
    std::filesystem::path state_root_;
    run_options current_;
};

} // namespace rivet

#endif // NJLANE314_RIVET_H_INCLUDED
