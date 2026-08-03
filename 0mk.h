#ifndef NJLANE314_0MK_H_INCLUDED
#define NJLANE314_0MK_H_INCLUDED

// 0mk.h - a small, content-aware dependency runner for C++17.
//
// 0mkfile syntax:
//
//   all <- report.pdf
//
//   result.json <- input.csv @local
//       ./analyse $< --output $@
//
//   publish! <- report.pdf @local
//       ./publish $<
//
// A rule produces exactly one file or directory-tree artifact. A recipe-less rule is an alias.
// A target ending in '!' is an always-run action. $@, $<, and $^ mean the
// private output, first input, and all file inputs respectively.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mk0 {

inline constexpr std::string_view version = "0.2.0";
inline constexpr std::string_view task_abi = "0mk-task-v2";
inline constexpr std::string_view executor_protocol = "0mk-exec-v1";

class error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class approval_required : public error {
public:
    using error::error;
};

class adapter_failure : public error {
public:
    using error::error;
};

enum class cache_policy { off, declared, hermetic };

inline std::string_view name(cache_policy selected) noexcept {
    switch (selected) {
    case cache_policy::off: return "off";
    case cache_policy::declared: return "declared";
    case cache_policy::hermetic: return "hermetic";
    }
    return "declared";
}

inline cache_policy parse_cache_policy(std::string_view selected) {
    if (selected == "off")
        return cache_policy::off;
    if (selected == "declared")
        return cache_policy::declared;
    if (selected == "hermetic")
        return cache_policy::hermetic;
    throw error("Unknown cache policy: " + std::string(selected));
}

enum class artifact_kind { file, tree };

inline std::string_view name(artifact_kind selected) noexcept {
    return selected == artifact_kind::file ? "file" : "tree";
}

struct artifact_manifest {
    artifact_kind kind = artifact_kind::file;
    std::string digest;
    std::uint64_t size = 0;
    std::uint32_t mode = 0;
};

inline bool operator==(const artifact_manifest& left, const artifact_manifest& right) {
    return left.kind == right.kind && left.digest == right.digest && left.size == right.size &&
           left.mode == right.mode;
}

inline bool operator!=(const artifact_manifest& left, const artifact_manifest& right) {
    return !(left == right);
}

enum class command_kind { argv, shell };

struct command_spec {
    command_kind kind = command_kind::argv;
    std::string raw;
    std::vector<std::string> argv;
    std::string shell;
};

enum class event_kind {
    plan,
    current,
    run,
    deferred,
    restored,
    done,
    target,
    inspect,
    cache,
    warning,
    log,
    failure
};

struct event {
    event_kind kind = event_kind::plan;
    std::string target;
    std::string profile;
    std::string message;
    std::map<std::string, std::string> facts;
    std::optional<artifact_manifest> artifact;
    std::chrono::milliseconds elapsed{0};
};

using event_sink = std::function<void(const event&)>;

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

inline bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

inline std::string normalize_reference(std::string value) {
    const std::filesystem::path path(value);
    for (const auto& component : path) {
        if (component == "..")
            return value; // lexical collapse across a symlink would change meaning
    }
    return path.lexically_normal().generic_string();
}

inline bool path_has_prefix(const std::filesystem::path& path,
                            const std::filesystem::path& prefix) {
    auto path_component = path.begin();
    for (auto prefix_component = prefix.begin(); prefix_component != prefix.end();
         ++prefix_component, ++path_component) {
        if (path_component == path.end() || *path_component != *prefix_component)
            return false;
    }
    return true;
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

inline std::string hex_encode(std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0f]);
    }
    return out;
}

inline std::string hex_decode(std::string_view value) {
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    if (value.size() % 2)
        throw error("Invalid hex-encoded value");
    std::string out;
    out.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int high = nibble(value[i]);
        const int low = nibble(value[i + 1]);
        if (high < 0 || low < 0)
            throw error("Invalid hex-encoded value");
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return out;
}

struct json {
    using array = std::vector<json>;
    using object = std::map<std::string, json, std::less<>>;
    using value = std::variant<std::nullptr_t, bool, double, std::string, array, object>;

    value data = nullptr;

    json() = default;
    json(std::nullptr_t) : data(nullptr) {}
    json(bool selected) : data(selected) {}
    json(double selected) : data(selected) {}
    json(int selected) : data(static_cast<double>(selected)) {}
    json(std::string selected) : data(std::move(selected)) {}
    json(const char* selected) : data(std::string(selected)) {}
    json(array selected) : data(std::move(selected)) {}
    json(object selected) : data(std::move(selected)) {}

    const object& as_object() const {
        const auto* selected = std::get_if<object>(&data);
        if (!selected)
            throw error("Expected a JSON object");
        return *selected;
    }

    const array& as_array() const {
        const auto* selected = std::get_if<array>(&data);
        if (!selected)
            throw error("Expected a JSON array");
        return *selected;
    }

    std::string as_string() const {
        const auto* selected = std::get_if<std::string>(&data);
        if (!selected)
            throw error("Expected a JSON string");
        return *selected;
    }

    bool as_bool() const {
        const auto* selected = std::get_if<bool>(&data);
        if (!selected)
            throw error("Expected a JSON boolean");
        return *selected;
    }

    double as_number() const {
        const auto* selected = std::get_if<double>(&data);
        if (!selected)
            throw error("Expected a JSON number");
        return *selected;
    }
};

class json_parser {
public:
    explicit json_parser(std::string_view text) : text_(text) {}

    json parse() {
        json result = value();
        whitespace();
        if (position_ != text_.size())
            fail("trailing data");
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw error("Invalid JSON at byte " + std::to_string(position_) + ": " +
                    std::string(message));
    }

    void whitespace() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\r' || text_[position_] == '\n'))
            ++position_;
    }

    bool consume(char expected) {
        whitespace();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    json value() {
        whitespace();
        if (position_ == text_.size())
            fail("expected a value");
        switch (text_[position_]) {
        case '{': return object();
        case '[': return array();
        case '"': return json(string());
        case 't': literal("true"); return json(true);
        case 'f': literal("false"); return json(false);
        case 'n': literal("null"); return json(nullptr);
        default: return number();
        }
    }

    json object() {
        ++position_;
        json::object result;
        if (consume('}'))
            return json(std::move(result));
        for (;;) {
            whitespace();
            if (position_ == text_.size() || text_[position_] != '"')
                fail("expected an object key");
            std::string key = string();
            if (!consume(':'))
                fail("expected ':'");
            if (!result.emplace(std::move(key), value()).second)
                fail("duplicate object key");
            if (consume('}'))
                return json(std::move(result));
            if (!consume(','))
                fail("expected ',' or '}'");
        }
    }

    json array() {
        ++position_;
        json::array result;
        if (consume(']'))
            return json(std::move(result));
        for (;;) {
            result.push_back(value());
            if (consume(']'))
                return json(std::move(result));
            if (!consume(','))
                fail("expected ',' or ']'");
        }
    }

    static unsigned hex_digit(char c) {
        if (c >= '0' && c <= '9')
            return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f')
            return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F')
            return static_cast<unsigned>(c - 'A' + 10);
        throw error("Invalid JSON Unicode escape");
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code <= 0x7f) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    std::string string() {
        ++position_;
        std::string result;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"')
                return result;
            if (static_cast<unsigned char>(c) < 0x20)
                fail("control byte in string");
            if (c != '\\') {
                result.push_back(c);
                continue;
            }
            if (position_ == text_.size())
                fail("truncated escape");
            const char escaped = text_[position_++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                if (position_ + 4 > text_.size())
                    fail("truncated Unicode escape");
                unsigned code = 0;
                for (int i = 0; i < 4; ++i)
                    code = (code << 4) | hex_digit(text_[position_++]);
                if (code >= 0xd800 && code <= 0xdfff)
                    fail("surrogate escapes are not supported");
                append_utf8(result, code);
                break;
            }
            default: fail("unknown string escape");
            }
        }
        fail("unterminated string");
    }

    json number() {
        const std::size_t begin = position_;
        if (text_[position_] == '-')
            ++position_;
        if (position_ == text_.size())
            fail("truncated number");
        if (text_[position_] == '0') {
            ++position_;
        } else {
            if (!std::isdigit(static_cast<unsigned char>(text_[position_])))
                fail("expected a value");
            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_])))
                ++position_;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            if (position_ == text_.size() ||
                !std::isdigit(static_cast<unsigned char>(text_[position_])))
                fail("invalid fraction");
            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_])))
                ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
                ++position_;
            if (position_ == text_.size() ||
                !std::isdigit(static_cast<unsigned char>(text_[position_])))
                fail("invalid exponent");
            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_])))
                ++position_;
        }
        const std::string token(text_.substr(begin, position_ - begin));
        char* end = nullptr;
        errno = 0;
        const double parsed = std::strtod(token.c_str(), &end);
        if (errno == ERANGE || !end || *end)
            fail("invalid number");
        return json(parsed);
    }

    void literal(std::string_view expected) {
        if (text_.substr(position_, expected.size()) != expected)
            fail("unknown literal");
        position_ += expected.size();
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

inline std::string json_escape(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(c) << std::dec;
            else
                out << static_cast<char>(c);
        }
    }
    out << '"';
    return out.str();
}

inline std::string json_dump(const json& selected) {
    if (std::holds_alternative<std::nullptr_t>(selected.data))
        return "null";
    if (const auto* value = std::get_if<bool>(&selected.data))
        return *value ? "true" : "false";
    if (const auto* value = std::get_if<double>(&selected.data)) {
        std::ostringstream out;
        out << std::setprecision(17) << *value;
        return out.str();
    }
    if (const auto* value = std::get_if<std::string>(&selected.data))
        return json_escape(*value);
    if (const auto* value = std::get_if<json::array>(&selected.data)) {
        std::string out = "[";
        for (std::size_t i = 0; i < value->size(); ++i) {
            if (i)
                out.push_back(',');
            out += json_dump((*value)[i]);
        }
        out.push_back(']');
        return out;
    }
    const auto& value = std::get<json::object>(selected.data);
    std::string out = "{";
    bool first = true;
    for (const auto& item : value) {
        if (!first)
            out.push_back(',');
        first = false;
        out += json_escape(item.first);
        out.push_back(':');
        out += json_dump(item.second);
    }
    out.push_back('}');
    return out;
}

inline const json* json_find(const json::object& object, std::string_view key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

inline std::string unique_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#if defined(__unix__) || defined(__APPLE__)
    const auto process = static_cast<long long>(::getpid());
#else
    const auto process = 0LL;
#endif
    return ".tmp." + std::to_string(process) + "." + std::to_string(now) + "." +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
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

inline void write_all(int descriptor, const char* data, std::size_t size,
                      const std::filesystem::path& target) {
#if defined(__unix__) || defined(__APPLE__)
    while (size) {
        const auto count = ::write(descriptor, data, size);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw error("Cannot write " + target.string() + ": " + std::strerror(errno));
        }
        data += count;
        size -= static_cast<std::size_t>(count);
    }
#else
    (void)descriptor;
    (void)data;
    (void)size;
    (void)target;
#endif
}

inline void write_atomically(const std::filesystem::path& target, std::string_view contents,
                             std::uint32_t mode = 0644) {
    ensure_parent(target);
    const std::filesystem::path temporary(target.string() + unique_suffix());
#if defined(__unix__) || defined(__APPLE__)
    const int descriptor = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC |
#ifdef O_NOFOLLOW
                                                      O_NOFOLLOW |
#endif
                                                      0,
                                  static_cast<mode_t>(mode));
    if (descriptor < 0)
        throw error("Cannot create temporary file for " + target.string() + ": " +
                    std::strerror(errno));
    try {
        write_all(descriptor, contents.data(), contents.size(), target);
        if (::fsync(descriptor) != 0)
            throw error("Cannot sync " + target.string() + ": " + std::strerror(errno));
        if (::close(descriptor) != 0)
            throw error("Cannot close " + target.string() + ": " + std::strerror(errno));
    } catch (...) {
        (void)::close(descriptor);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
#else
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw error("Cannot create temporary file for " + target.string());
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!output)
            throw error("Cannot write " + target.string());
    }
#endif
    try {
        replace_file(temporary, target);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

inline void copy_atomically(const std::filesystem::path& source,
                            const std::filesystem::path& target) {
    ensure_parent(target);
    const std::filesystem::path temporary(target.string() + unique_suffix());
#if defined(__unix__) || defined(__APPLE__)
    const int input = ::open(source.c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_NOFOLLOW
                                             | O_NOFOLLOW
#endif
    );
    if (input < 0)
        throw error("Cannot open artifact " + source.string() + ": " + std::strerror(errno));
    struct stat information {};
    if (::fstat(input, &information) != 0 || !S_ISREG(information.st_mode)) {
        (void)::close(input);
        throw error("Artifact is not a regular file: " + source.string());
    }
    const int output = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC |
#ifdef O_NOFOLLOW
                                                   O_NOFOLLOW |
#endif
                                                   0,
                              0600);
    if (output < 0) {
        (void)::close(input);
        throw error("Cannot create artifact temporary for " + target.string() + ": " +
                    std::strerror(errno));
    }
    try {
        std::array<char, 64 * 1024> buffer{};
        for (;;) {
            const auto count = ::read(input, buffer.data(), buffer.size());
            if (count == 0)
                break;
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                throw error("Cannot read artifact " + source.string() + ": " +
                            std::strerror(errno));
            }
            write_all(output, buffer.data(), static_cast<std::size_t>(count), target);
        }
        if (::fsync(output) != 0)
            throw error("Cannot sync artifact " + target.string() + ": " + std::strerror(errno));
        const int input_close = ::close(input);
        const int output_close = ::close(output);
        if (input_close != 0 || output_close != 0)
            throw error("Cannot close artifact temporary for " + target.string());
    } catch (...) {
        (void)::close(input);
        (void)::close(output);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
#else
    std::error_code ec;
    std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::none, ec);
    if (ec)
        throw error("Cannot copy artifact to " + target.string() + ": " + ec.message());
#endif
    try {
        replace_file(temporary, target);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

class workspace_lock {
public:
    workspace_lock(const std::filesystem::path& path, bool enabled, bool shared = false) {
#if defined(__unix__) || defined(__APPLE__)
        if (!enabled)
            return;
        ensure_parent(path);
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
        if (descriptor_ < 0)
            throw error("Cannot open workspace lock: " + path.string());
        while (::flock(descriptor_, shared ? LOCK_SH : LOCK_EX) != 0) {
            if (errno != EINTR) {
                ::close(descriptor_);
                descriptor_ = -1;
                throw error("Cannot acquire workspace lock: " + path.string());
            }
        }
#else
        (void)path;
        (void)enabled;
        (void)shared;
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
    std::string contents;
    for (const auto& field : fields) {
        if (field.first.find_first_of("=\r\n") != std::string::npos ||
            field.second.find_first_of("\r\n") != std::string::npos)
            throw error("Receipt fields must be line-oriented");
        contents += field.first;
        contents.push_back('=');
        contents += field.second;
        contents.push_back('\n');
    }
    write_atomically(path, contents);
}

inline std::uint32_t path_mode(const std::filesystem::path& path) {
#if defined(__unix__) || defined(__APPLE__)
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0)
        throw error("Cannot inspect artifact " + path.string() + ": " + std::strerror(errno));
    if (S_ISLNK(information.st_mode))
        throw error("Symlink artifacts are not supported: " + path.string());
    return static_cast<std::uint32_t>(information.st_mode & 07777U);
#else
    const auto permissions = std::filesystem::symlink_status(path).permissions();
    return static_cast<std::uint32_t>(permissions) & 07777U;
#endif
}

inline void set_mode(const std::filesystem::path& path, std::uint32_t mode) {
#if defined(__unix__) || defined(__APPLE__)
    if (::chmod(path.c_str(), static_cast<mode_t>(mode)) != 0)
        throw error("Cannot set artifact mode for " + path.string() + ": " +
                    std::strerror(errno));
#else
    std::filesystem::permissions(path, static_cast<std::filesystem::perms>(mode),
                                 std::filesystem::perm_options::replace);
#endif
}

inline void make_removable(const std::filesystem::path& path, std::error_code& failure) {
    failure.clear();
    const auto status = std::filesystem::symlink_status(path, failure);
    if (failure == std::make_error_code(std::errc::no_such_file_or_directory)) {
        failure.clear();
        return;
    }
    if (failure || !std::filesystem::exists(status) || std::filesystem::is_symlink(status))
        return;
    if (!std::filesystem::is_directory(status))
        return;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::add, failure);
    if (failure)
        return;
    std::filesystem::directory_iterator iterator(path, failure);
    const std::filesystem::directory_iterator end;
    while (!failure && iterator != end) {
        make_removable(iterator->path(), failure);
        iterator.increment(failure);
    }
}

inline void remove_tree(const std::filesystem::path& path, std::error_code& failure) {
    make_removable(path, failure);
    if (!failure)
        (void)std::filesystem::remove_all(path, failure);
}

struct tree_entry {
    artifact_kind kind = artifact_kind::file;
    std::string path;
    std::string digest;
    std::uint64_t size = 0;
    std::uint32_t mode = 0;
    std::filesystem::path source;
};

struct captured_artifact {
    artifact_manifest manifest;
    std::vector<tree_entry> entries;
    std::filesystem::path source;
};

inline std::string serialize_tree(const std::vector<tree_entry>& entries) {
    std::string output = "0MK_TREE_V1\n";
    for (const auto& entry : entries) {
        output += entry.kind == artifact_kind::file ? "F " : "D ";
        output += hex_encode(entry.path);
        output.push_back(' ');
        output += std::to_string(entry.mode);
        output.push_back(' ');
        output += std::to_string(entry.size);
        output.push_back(' ');
        output += entry.digest.empty() ? "-" : entry.digest;
        output.push_back('\n');
    }
    return output;
}

inline captured_artifact capture_artifact(const std::filesystem::path& path) {
    const auto status = std::filesystem::symlink_status(path);
    if (std::filesystem::is_symlink(status))
        throw error("Symlink artifacts are not supported: " + path.string());
    captured_artifact captured;
    captured.source = path;
    captured.manifest.mode = path_mode(path);
    if (std::filesystem::is_regular_file(status)) {
        captured.manifest.kind = artifact_kind::file;
        captured.manifest.digest = hash_file(path);
        std::error_code size_error;
        captured.manifest.size = std::filesystem::file_size(path, size_error);
        if (size_error)
            throw error("Cannot read artifact size for " + path.string() + ": " +
                        size_error.message());
        return captured;
    }
    if (!std::filesystem::is_directory(status))
        throw error("Artifact is neither a regular file nor a directory tree: " + path.string());

    captured.manifest.kind = artifact_kind::tree;
    std::vector<std::filesystem::path> paths;
    std::error_code walk_error;
    for (std::filesystem::recursive_directory_iterator iterator(path, walk_error), end;
         !walk_error && iterator != end; iterator.increment(walk_error))
        paths.push_back(iterator->path());
    if (walk_error)
        throw error("Cannot scan artifact tree " + path.string() + ": " + walk_error.message());
    std::sort(paths.begin(), paths.end(), [&](const auto& left, const auto& right) {
        return left.lexically_relative(path).generic_string() <
               right.lexically_relative(path).generic_string();
    });
    for (const auto& item : paths) {
        const auto item_status = std::filesystem::symlink_status(item);
        if (std::filesystem::is_symlink(item_status))
            throw error("Symlinks are not supported in artifact trees: " + item.string());
        tree_entry entry;
        entry.path = item.lexically_relative(path).generic_string();
        entry.mode = path_mode(item);
        entry.source = item;
        if (std::filesystem::is_directory(item_status)) {
            entry.kind = artifact_kind::tree;
        } else if (std::filesystem::is_regular_file(item_status)) {
            entry.kind = artifact_kind::file;
            entry.digest = hash_file(item);
            std::error_code size_error;
            entry.size = std::filesystem::file_size(item, size_error);
            if (size_error)
                throw error("Cannot read artifact size for " + item.string() + ": " +
                            size_error.message());
            captured.manifest.size += entry.size;
        } else {
            throw error("Unsupported entry in artifact tree: " + item.string());
        }
        captured.entries.push_back(std::move(entry));
    }
    captured.manifest.digest = hash_text(serialize_tree(captured.entries));
    return captured;
}

inline std::filesystem::path blob_path(const std::filesystem::path& state_root,
                                       std::string_view digest) {
    if (!valid_digest(digest))
        throw error("Invalid artifact digest");
    const auto directory = state_root / "objects" / std::string(digest.substr(0, 2));
    const auto status = std::filesystem::symlink_status(directory);
    if (std::filesystem::exists(status) &&
        (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)))
        throw error("Unsafe cache object directory: " + directory.string());
    return directory / std::string(digest.substr(2));
}

inline std::filesystem::path tree_path(const std::filesystem::path& state_root,
                                       std::string_view digest) {
    if (!valid_digest(digest))
        throw error("Invalid tree digest");
    return state_root / "trees" / (std::string(digest) + ".txt");
}

inline bool valid_tree_manifest(const std::filesystem::path& path, std::string_view digest) {
    const auto status = std::filesystem::symlink_status(path);
    return std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status) &&
           path_mode(path) == 0444U && hash_file(path) == digest;
}

inline bool valid_blob(const std::filesystem::path& path, std::string_view digest) {
    const auto status = std::filesystem::symlink_status(path);
    return std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status) &&
           path_mode(path) == 0444U && hash_file(path) == digest;
}

inline void import_blob(const std::filesystem::path& source,
                        const std::filesystem::path& state_root, std::string_view digest) {
    const auto destination = blob_path(state_root, digest);
    workspace_lock lock(state_root / "locks" / ("object-" + std::string(digest)), true);
    if (valid_blob(destination, digest))
        return;
    std::error_code remove_error;
    std::filesystem::remove(destination, remove_error);
    if (remove_error)
        throw error("Cannot replace corrupt cache object: " + remove_error.message());
    copy_atomically(source, destination);
    set_mode(destination, 0444);
    if (!valid_blob(destination, digest)) {
        std::filesystem::remove(destination, remove_error);
        throw error("Artifact changed while it was imported: " + source.string());
    }
}

inline captured_artifact import_artifact(const std::filesystem::path& source,
                                         const std::filesystem::path& state_root) {
    captured_artifact captured = capture_artifact(source);
    if (captured.manifest.kind == artifact_kind::file) {
        import_blob(source, state_root, captured.manifest.digest);
        return captured;
    }
    for (const auto& entry : captured.entries)
        if (entry.kind == artifact_kind::file)
            import_blob(entry.source, state_root, entry.digest);
    const captured_artifact after = capture_artifact(source);
    if (after.manifest != captured.manifest)
        throw error("Artifact tree changed while it was imported: " + source.string());
    const std::string serialized = serialize_tree(captured.entries);
    const auto destination = tree_path(state_root, captured.manifest.digest);
    workspace_lock lock(state_root / "locks" / ("tree-" + captured.manifest.digest), true);
    const bool current = valid_tree_manifest(destination, captured.manifest.digest);
    if (!current)
        write_atomically(destination, serialized, 0444);
    if (!valid_tree_manifest(destination, captured.manifest.digest))
        throw error("Cannot store tree manifest " + captured.manifest.digest);
    return captured;
}

inline std::vector<tree_entry> read_tree(const std::filesystem::path& state_root,
                                         std::string_view digest) {
    const auto path = tree_path(state_root, digest);
    if (!valid_tree_manifest(path, digest))
        throw error("Cached tree manifest is missing or corrupt: " + std::string(digest));
    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line) || line != "0MK_TREE_V1")
        throw error("Invalid cached tree manifest: " + path.string());
    std::vector<tree_entry> entries;
    std::set<std::string> names;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        char kind = 0;
        std::string encoded;
        std::uint64_t mode = 0;
        std::uint64_t size = 0;
        std::string entry_digest;
        std::string extra;
        if (!(fields >> kind >> encoded >> mode >> size >> entry_digest) || fields >> extra ||
            (kind != 'F' && kind != 'D') || mode > 07777U)
            throw error("Invalid cached tree entry: " + path.string());
        tree_entry entry;
        entry.kind = kind == 'F' ? artifact_kind::file : artifact_kind::tree;
        entry.path = hex_decode(encoded);
        const std::filesystem::path relative(entry.path);
        if (relative.empty() || relative.is_absolute() || relative.lexically_normal() != relative)
            throw error("Unsafe cached tree entry: " + entry.path);
        for (const auto& component : relative)
            if (component == "..")
                throw error("Unsafe cached tree entry: " + entry.path);
        if (!names.insert(entry.path).second)
            throw error("Duplicate cached tree entry: " + entry.path);
        entry.mode = static_cast<std::uint32_t>(mode);
        entry.size = size;
        if (entry.kind == artifact_kind::file) {
            if (!valid_digest(entry_digest))
                throw error("Invalid cached tree object digest");
            entry.digest = std::move(entry_digest);
        } else if (entry_digest != "-" || size != 0) {
            throw error("Invalid cached tree directory entry");
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

inline bool artifact_available(const artifact_manifest& manifest,
                               const std::filesystem::path& state_root) {
    try {
        if (manifest.kind == artifact_kind::file) {
            const auto path = blob_path(state_root, manifest.digest);
            if (!valid_blob(path, manifest.digest))
                return false;
            std::error_code size_error;
            const auto size = std::filesystem::file_size(path, size_error);
            return !size_error && size == manifest.size;
        }
        const auto entries = read_tree(state_root, manifest.digest);
        std::uint64_t size = 0;
        for (const auto& entry : entries) {
            if (entry.kind == artifact_kind::file) {
                const auto path = blob_path(state_root, entry.digest);
                if (!valid_blob(path, entry.digest))
                    return false;
                std::error_code size_error;
                const auto entry_size = std::filesystem::file_size(path, size_error);
                if (size_error || entry_size != entry.size)
                    return false;
                size += entry.size;
            }
        }
        return size == manifest.size;
    } catch (...) {
        return false;
    }
}

inline std::uint32_t readonly_mode(std::uint32_t mode) {
    return mode & ~static_cast<std::uint32_t>(0222U);
}

inline void materialize_artifact(const artifact_manifest& manifest,
                                 const std::filesystem::path& state_root,
                                 const std::filesystem::path& target, bool read_only = false) {
    if (!artifact_available(manifest, state_root))
        throw error("Cached artifact is missing or corrupt: " + manifest.digest);
    if (manifest.kind == artifact_kind::file) {
        const bool had_directory =
            std::filesystem::is_directory(std::filesystem::symlink_status(target));
        const std::filesystem::path backup(target.string() + unique_suffix() + ".old");
        std::error_code filesystem_error;
        if (had_directory) {
            std::filesystem::rename(target, backup, filesystem_error);
            if (filesystem_error)
                throw error("Cannot stage replacement of " + target.string() + ": " +
                            filesystem_error.message());
        }
        try {
            copy_atomically(blob_path(state_root, manifest.digest), target);
        } catch (...) {
            if (had_directory) {
                std::error_code ignored;
                std::filesystem::rename(backup, target, ignored);
            }
            throw;
        }
        if (had_directory) {
            remove_tree(backup, filesystem_error);
            if (filesystem_error)
                throw error("Cannot remove replaced target " + backup.string() + ": " +
                            filesystem_error.message());
        }
        set_mode(target, read_only ? readonly_mode(manifest.mode) : manifest.mode);
        artifact_manifest expected = manifest;
        if (read_only)
            expected.mode = readonly_mode(expected.mode);
        if (capture_artifact(target).manifest != expected)
            throw error("Published file failed verification: " + target.string());
        return;
    }

    const auto entries = read_tree(state_root, manifest.digest);
    const std::filesystem::path staging(target.string() + unique_suffix());
    const std::filesystem::path backup(target.string() + unique_suffix() + ".old");
    std::error_code filesystem_error;
    std::filesystem::create_directories(staging, filesystem_error);
    if (filesystem_error)
        throw error("Cannot stage artifact tree: " + filesystem_error.message());
    try {
        for (const auto& entry : entries) {
            const auto destination = staging / std::filesystem::path(entry.path);
            if (entry.kind == artifact_kind::tree) {
                std::filesystem::create_directories(destination, filesystem_error);
                if (filesystem_error)
                    throw error("Cannot create artifact directory: " + filesystem_error.message());
            } else {
                ensure_parent(destination);
                copy_atomically(blob_path(state_root, entry.digest), destination);
                set_mode(destination, read_only ? readonly_mode(entry.mode) : entry.mode);
            }
        }
        for (auto iterator = entries.rbegin(); iterator != entries.rend(); ++iterator)
            if (iterator->kind == artifact_kind::tree)
                set_mode(staging / std::filesystem::path(iterator->path),
                         read_only ? readonly_mode(iterator->mode) : iterator->mode);
        set_mode(staging, read_only ? readonly_mode(manifest.mode) : manifest.mode);

        const bool had_target = std::filesystem::exists(std::filesystem::symlink_status(target));
        if (had_target) {
            std::filesystem::rename(target, backup, filesystem_error);
            if (filesystem_error)
                throw error("Cannot stage replacement of " + target.string() + ": " +
                            filesystem_error.message());
        }
        std::filesystem::rename(staging, target, filesystem_error);
        if (filesystem_error) {
            if (had_target) {
                std::error_code ignored;
                std::filesystem::rename(backup, target, ignored);
            }
            throw error("Cannot publish artifact tree " + target.string() + ": " +
                        filesystem_error.message());
        }
        if (had_target) {
            remove_tree(backup, filesystem_error);
            if (filesystem_error)
                throw error("Cannot remove replaced target " + backup.string() + ": " +
                            filesystem_error.message());
        }
        artifact_manifest expected = manifest;
        if (read_only) {
            expected.mode = readonly_mode(expected.mode);
            auto readonly_entries = entries;
            for (auto& entry : readonly_entries)
                entry.mode = readonly_mode(entry.mode);
            expected.digest = hash_text(serialize_tree(readonly_entries));
        }
        if (capture_artifact(target).manifest != expected)
            throw error("Published tree failed verification: " + target.string());
    } catch (...) {
        std::error_code ignored;
        remove_tree(staging, ignored);
        throw;
    }
}

inline bool path_matches(const std::filesystem::path& path,
                         const artifact_manifest& manifest) {
    try {
        return capture_artifact(path).manifest == manifest;
    } catch (...) {
        return false;
    }
}

struct process_result {
    int exit_code = 0;
    std::string output;
    std::string failure;
};

#if defined(__unix__) || defined(__APPLE__)
inline std::array<std::atomic<int>, 256> active_process_groups{};
inline std::atomic<int> received_signal{0};

inline void forward_signal(int selected) {
    received_signal.store(selected, std::memory_order_relaxed);
    for (auto& slot : active_process_groups) {
        const int process_group = slot.load(std::memory_order_relaxed);
        if (process_group > 0)
            (void)::kill(-process_group, selected);
    }
}

class signal_scope {
public:
    signal_scope() {
        received_signal.store(0, std::memory_order_relaxed);
        struct sigaction action {};
        action.sa_handler = forward_signal;
        (void)sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (::sigaction(SIGINT, &action, &old_interrupt_) != 0 ||
            ::sigaction(SIGTERM, &action, &old_terminate_) != 0)
            throw error("Cannot install signal handlers: " + std::string(std::strerror(errno)));
        installed_ = true;
    }

    signal_scope(const signal_scope&) = delete;
    signal_scope& operator=(const signal_scope&) = delete;

    ~signal_scope() {
        if (installed_) {
            (void)::sigaction(SIGINT, &old_interrupt_, nullptr);
            (void)::sigaction(SIGTERM, &old_terminate_, nullptr);
        }
    }

private:
    struct sigaction old_interrupt_ {};
    struct sigaction old_terminate_ {};
    bool installed_ = false;
};

class child_registration {
public:
    explicit child_registration(pid_t process_group) {
        for (std::size_t i = 0; i < active_process_groups.size(); ++i) {
            int empty = 0;
            if (active_process_groups[i].compare_exchange_strong(
                    empty, static_cast<int>(process_group), std::memory_order_relaxed)) {
                index_ = i;
                const int pending = received_signal.load(std::memory_order_relaxed);
                if (pending)
                    (void)::kill(-process_group, pending);
                return;
            }
        }
        throw error("Too many concurrent recipe processes");
    }

    child_registration(const child_registration&) = delete;
    child_registration& operator=(const child_registration&) = delete;

    ~child_registration() {
        if (index_)
            active_process_groups[*index_].store(0, std::memory_order_relaxed);
    }

private:
    std::optional<std::size_t> index_;
};
#else
class signal_scope {
public:
    signal_scope() = default;
};
inline std::atomic<int> received_signal{0};
#endif

inline std::filesystem::path find_executable(std::string_view program,
                                             const std::filesystem::path& directory = {}) {
    const std::filesystem::path requested(program);
    if (requested.has_parent_path()) {
        const auto candidate = requested.is_absolute() ? requested : directory / requested;
#if defined(__unix__) || defined(__APPLE__)
        if (::access(candidate.c_str(), X_OK) == 0) {
            std::error_code canonical_error;
            const auto canonical = std::filesystem::canonical(candidate, canonical_error);
            if (!canonical_error && std::filesystem::is_regular_file(canonical))
                return canonical;
        }
#else
        if (std::filesystem::is_regular_file(candidate))
            return std::filesystem::absolute(candidate);
#endif
        return {};
    }
    const char* raw_path = std::getenv("PATH");
    const std::string path = raw_path ? raw_path : "";
    std::size_t begin = 0;
    for (;;) {
        const auto end = path.find(':', begin);
        const std::string component = path.substr(begin, end == std::string::npos ? end : end - begin);
        const auto candidate = std::filesystem::path(component.empty() ? "." : component) / requested;
#if defined(__unix__) || defined(__APPLE__)
        if (::access(candidate.c_str(), X_OK) == 0) {
            std::error_code canonical_error;
            const auto canonical = std::filesystem::canonical(candidate, canonical_error);
            if (!canonical_error && std::filesystem::is_regular_file(canonical))
                return canonical;
        }
#else
        if (std::filesystem::is_regular_file(candidate))
            return std::filesystem::absolute(candidate);
#endif
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return {};
}

inline process_result spawn(const std::vector<std::string>& arguments,
                            const std::filesystem::path& directory, int input_descriptor = -1,
                            int output_descriptor = -1) {
    process_result result;
    if (arguments.empty()) {
        result.exit_code = 127;
        result.failure = "empty command";
        return result;
    }
#if defined(__unix__) || defined(__APPLE__)
    if (const int pending = received_signal.load(std::memory_order_relaxed)) {
        result.exit_code = 128 + pending;
        result.failure = "cancelled by signal " + std::to_string(pending);
        return result;
    }
    int error_pipe[2] = {-1, -1};
    if (::pipe(error_pipe) != 0)
        throw error("Cannot create process error pipe: " + std::string(std::strerror(errno)));
    (void)::fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC);
    std::vector<char*> raw;
    raw.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
        raw.push_back(const_cast<char*>(argument.c_str()));
    raw.push_back(nullptr);
    const pid_t child = ::fork();
    if (child < 0) {
        const int saved = errno;
        (void)::close(error_pipe[0]);
        (void)::close(error_pipe[1]);
        throw error("Cannot fork: " + std::string(std::strerror(saved)));
    }
    if (child == 0) {
        (void)::close(error_pipe[0]);
        (void)::setpgid(0, 0);
        struct sigaction default_action {};
        default_action.sa_handler = SIG_DFL;
        (void)sigemptyset(&default_action.sa_mask);
        (void)::sigaction(SIGINT, &default_action, nullptr);
        (void)::sigaction(SIGTERM, &default_action, nullptr);
        if (input_descriptor >= 0 && ::dup2(input_descriptor, STDIN_FILENO) < 0) {
            const int saved = errno;
            (void)::write(error_pipe[1], &saved, sizeof(saved));
            ::_exit(126);
        }
        if (output_descriptor >= 0 && ::dup2(output_descriptor, STDOUT_FILENO) < 0) {
            const int saved = errno;
            (void)::write(error_pipe[1], &saved, sizeof(saved));
            ::_exit(126);
        }
        if (!directory.empty() && ::chdir(directory.c_str()) != 0) {
            const int saved = errno;
            (void)::write(error_pipe[1], &saved, sizeof(saved));
            ::_exit(126);
        }
        ::execvp(raw.front(), raw.data());
        const int saved = errno;
        (void)::write(error_pipe[1], &saved, sizeof(saved));
        ::_exit(saved == ENOENT ? 127 : 126);
    }
    if (::setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        const int saved = errno;
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, nullptr, 0);
        (void)::close(error_pipe[0]);
        (void)::close(error_pipe[1]);
        throw error("Cannot create recipe process group: " + std::string(std::strerror(saved)));
    }
    child_registration registration(child);
    (void)::close(error_pipe[1]);
    int execution_error = 0;
    ssize_t error_count = 0;
    do {
        error_count = ::read(error_pipe[0], &execution_error, sizeof(execution_error));
    } while (error_count < 0 && errno == EINTR);
    (void)::close(error_pipe[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            throw error("Cannot wait for child process: " + std::string(std::strerror(errno)));
    }
    if (error_count == static_cast<ssize_t>(sizeof(execution_error)))
        result.failure = arguments.front() + ": " + std::strerror(execution_error);
    if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
        result.failure = "terminated by signal " + std::to_string(WTERMSIG(status));
    } else
        result.exit_code = 125;
#else
    std::string command = "cd " + shell_quote(directory.string()) + " &&";
    for (const auto& argument : arguments)
        command += " " + shell_quote(argument);
    const int status = std::system(command.c_str());
    result.exit_code = status == 0 ? 0 : 1;
    (void)input_descriptor;
    (void)output_descriptor;
#endif
    return result;
}

inline process_result request_process(const std::vector<std::string>& arguments,
                                      const std::filesystem::path& directory,
                                      std::string_view request) {
    std::FILE* input = std::tmpfile();
    std::FILE* output = std::tmpfile();
    if (!input || !output) {
        if (input)
            std::fclose(input);
        if (output)
            std::fclose(output);
        throw error("Cannot create executor protocol streams");
    }
    if (std::fwrite(request.data(), 1, request.size(), input) != request.size() ||
        std::fputc('\n', input) == EOF || std::fflush(input) != 0 || std::fseek(input, 0, SEEK_SET) != 0) {
        std::fclose(input);
        std::fclose(output);
        throw error("Cannot prepare executor request");
    }
    process_result result = spawn(arguments, directory, ::fileno(input), ::fileno(output));
    if (std::fflush(output) != 0 || std::fseek(output, 0, SEEK_SET) != 0) {
        std::fclose(input);
        std::fclose(output);
        throw error("Cannot read executor response");
    }
    std::array<char, 4096> buffer{};
    for (;;) {
        const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), output);
        result.output.append(buffer.data(), count);
        if (count != buffer.size()) {
            if (std::ferror(output)) {
                std::fclose(input);
                std::fclose(output);
                throw error("Cannot read executor response");
            }
            break;
        }
    }
    std::fclose(input);
    std::fclose(output);
    return result;
}

} // namespace detail

struct artifact_input {
    std::string logical_name;
    std::filesystem::path local_path;
    artifact_manifest artifact;
    bool available = true;
    bool provisional = false;
    bool materialize = true;
};

struct invocation {
    std::string target;
    std::string task_key;
    std::string attempt;
    std::vector<command_spec> commands;
    std::vector<artifact_input> inputs;
    std::filesystem::path output;
    std::filesystem::path workspace;
    std::filesystem::path source_root;
    std::filesystem::path working_directory;
    std::vector<std::pair<std::string, std::string>> environment;
    cache_policy cache = cache_policy::declared;
    bool submit = false;
};

struct plan {
    std::map<std::string, std::string> facts;
    std::vector<std::string> warnings;
    bool requires_submit = false;
    std::optional<double> estimated_cost_usd;
};

struct result {
    int exit_code = 0;
    std::map<std::string, std::string> metadata;
    std::string handle;
    bool success() const noexcept { return exit_code == 0; }
};

using log_sink = std::function<void(std::string_view)>;

struct executor {
    std::string cache_identity;
    cache_policy guarantee = cache_policy::declared;
    std::function<std::string(const invocation&)> fingerprint;
    std::function<mk0::plan(const invocation&)> plan;
    std::function<mk0::result(const invocation&, log_sink)> run;
    bool compatibility_shell = false;
    bool external = false;
    std::string executable;
};

enum class operation {
    run,
    plan,
    why,
    inspect,
    targets,
    cache_verify,
    cache_du,
    cache_gc
};

enum class exit_status : int {
    success = 0,
    task_failed = 1,
    usage = 2,
    approval_required = 3,
    cache_corrupt = 4
};

struct run_options {
    std::filesystem::path file = "0mkfile";
    std::filesystem::path directory;
    std::filesystem::path state_dir;
    std::vector<std::string> targets;
    operation command = operation::run;
    bool dry_run = false;
    bool force = false;
    bool why = false;
    bool json = false;
    bool submit = false;
    bool keep_going = false;
    std::size_t jobs = 1;
    cache_policy cache = cache_policy::declared;
    std::vector<std::string> environment;
    event_sink events;
};

inline executor local_executor(std::string identity = "local-argv-v1") {
    executor out;
    out.cache_identity = std::move(identity);
    out.guarantee = cache_policy::declared;
    out.fingerprint = [base = out.cache_identity](const invocation& call) {
        detail::fingerprint identity_digest;
        identity_digest.add(base);
        for (const auto& command : call.commands) {
            std::string program;
            if (command.kind == command_kind::shell) {
                program = "/bin/sh";
            } else if (!command.argv.empty()) {
                program = command.argv.front();
            }
            identity_digest.add(program);
            const auto executable = detail::find_executable(program, call.source_root);
            if (executable.empty()) {
                identity_digest.add("missing");
            } else {
                identity_digest.add(executable.string());
                identity_digest.add(detail::hash_file(executable));
                identity_digest.add(std::to_string(detail::path_mode(executable)));
            }
        }
        return identity_digest.finish();
    };
    out.plan = [](const invocation&) {
        mk0::plan value;
        value.facts.emplace("executor", "local");
        return value;
    };
    out.run = [](const invocation& call, log_sink log) {
        result value;
        for (const auto& command : call.commands) {
            std::vector<std::string> arguments;
            if (command.kind == command_kind::shell)
                arguments = {"/bin/sh", "-c", command.shell};
            else
                arguments = command.argv;
            if (arguments.empty()) {
                value.exit_code = 127;
                value.metadata.emplace("error", "empty command");
                return value;
            }
            std::string display;
            for (const auto& argument : arguments) {
                if (!display.empty())
                    display.push_back(' ');
                display += detail::shell_quote(argument);
            }
            log("$ " + display);
#if defined(__unix__) || defined(__APPLE__)
            const auto executed =
                detail::spawn(arguments, call.working_directory, -1, STDERR_FILENO);
#else
            const auto executed = detail::spawn(arguments, call.working_directory);
#endif
            value.exit_code = executed.exit_code;
            if (!executed.failure.empty())
                value.metadata["error"] = executed.failure;
            if (value.exit_code != 0)
                return value;
        }
        return value;
    };
    return out;
}

inline executor shell_executor(std::string identity = "shell-v1") {
    executor out = local_executor(std::move(identity));
    out.compatibility_shell = true;
    out.plan = [](const invocation&) {
        mk0::plan value;
        value.facts.emplace("executor", "shell-compat");
        value.warnings.emplace_back("@shell is deprecated; invoke sh -c explicitly");
        return value;
    };
    return out;
}

namespace detail {

inline json artifact_json(const artifact_manifest& artifact) {
    return json::object{{"digest", artifact.digest},
                        {"kind", std::string(name(artifact.kind))},
                        {"mode", std::to_string(artifact.mode)},
                        {"size", std::to_string(artifact.size)}};
}

inline json invocation_json(const invocation& call) {
    json::array commands;
    for (const auto& command : call.commands) {
        json::object encoded{{"kind", command.kind == command_kind::argv ? "argv" : "shell"}};
        if (command.kind == command_kind::argv) {
            json::array arguments;
            for (const auto& argument : command.argv)
                arguments.emplace_back(argument);
            encoded.emplace("argv", std::move(arguments));
        } else {
            encoded.emplace("command", command.shell);
        }
        encoded.emplace("raw", command.raw);
        commands.emplace_back(std::move(encoded));
    }
    json::array inputs;
    for (const auto& input : call.inputs) {
        inputs.emplace_back(json::object{{"artifact", artifact_json(input.artifact)},
                                         {"available", input.available},
                                         {"name", input.logical_name},
                                         {"path", input.local_path.string()},
                                         {"provisional", input.provisional}});
    }
    json::array environment;
    for (const auto& item : call.environment)
        environment.emplace_back(json::object{{"digest", item.second}, {"name", item.first}});
    return json::object{{"cache_policy", std::string(name(call.cache))},
                        {"commands", std::move(commands)},
                        {"environment", std::move(environment)},
                        {"inputs", std::move(inputs)},
                        {"attempt", call.attempt},
                        {"key", call.task_key},
                        {"output", call.output.string()},
                        {"source_root", call.source_root.string()},
                        {"target", call.target},
                        {"workspace", call.workspace.string()},
                        {"working_directory", call.working_directory.string()}};
}

inline std::string protocol_request(std::string_view operation, std::string_view profile,
                                    const invocation* call = nullptr,
                                    std::string_view handle = {}) {
    json::object request{{"operation", std::string(operation)},
                         {"profile", std::string(profile)},
                         {"protocol", std::string(executor_protocol)}};
    if (call) {
        request.emplace("submit", call->submit);
        request.emplace("task", invocation_json(*call));
    }
    if (!handle.empty())
        request.emplace("handle", std::string(handle));
    return json_dump(json(std::move(request)));
}

inline json::object protocol_response(const process_result& response, std::string_view operation,
                                      std::string_view profile) {
    if (response.exit_code != 0) {
        std::string diagnostic = response.failure;
        try {
            const auto failure = json_parser(response.output).parse().as_object();
            if (const auto* encoded = json_find(failure, "error"))
                if (const auto* message = json_find(encoded->as_object(), "message"))
                    diagnostic = message->as_string();
        } catch (...) {
            // A failed or signalled adapter may be unable to emit a JSON response.
        }
        const std::string message = "Executor '" + std::string(profile) + "' " +
                                    std::string(operation) + " failed with exit code " +
                                    std::to_string(response.exit_code) +
                                    (diagnostic.empty() ? "" : ": " + diagnostic);
        if (response.exit_code == 77)
            throw approval_required(message);
        throw adapter_failure(message);
    }
    json::object parsed;
    try {
        parsed = json_parser(response.output).parse().as_object();
    } catch (const std::exception& failure) {
        throw adapter_failure("Executor '" + std::string(profile) + "' " +
                              std::string(operation) + " returned invalid JSON: " +
                              failure.what());
    }
    const auto* protocol = json_find(parsed, "protocol");
    try {
        if (!protocol || protocol->as_string() != executor_protocol)
            throw adapter_failure("Executor '" + std::string(profile) +
                                  "' returned an incompatible protocol");
    } catch (const adapter_failure&) {
        throw;
    } catch (const std::exception&) {
        throw adapter_failure("Executor '" + std::string(profile) +
                              "' returned an incompatible protocol");
    }
    return parsed;
}

inline std::map<std::string, std::string> json_string_map(const json* value) {
    std::map<std::string, std::string> output;
    if (!value)
        return output;
    for (const auto& item : value->as_object())
        output.emplace(item.first, item.second.as_string());
    return output;
}

inline std::vector<std::string> json_string_array(const json* value) {
    std::vector<std::string> output;
    if (!value)
        return output;
    for (const auto& item : value->as_array())
        output.push_back(item.as_string());
    return output;
}

} // namespace detail

class engine {
public:
    engine();

    void profile(std::string name, executor implementation);
    int cli(int argc, char** argv, std::filesystem::path default_file = "0mkfile");
    int run(run_options selected = {});
    static void help(std::ostream& out);

private:
    struct rule {
        std::string target;
        std::vector<std::string> dependencies;
        std::string profile = "local";
        std::vector<std::string> recipes;
        bool action = false;
        std::size_t line = 0;
    };

    struct built {
        std::string identity;
        std::optional<artifact_manifest> artifact;
        bool would_change = false;
        bool provisional = false;
    };

    struct receipt_input {
        std::string name;
        std::string identity;
        std::optional<artifact_manifest> artifact;
    };

    struct receipt_data {
        bool valid = false;
        std::string task_key;
        std::string target;
        std::string profile;
        std::string executor_identity;
        std::string canonical_command;
        std::string command_digest;
        std::string environment_digest;
        cache_policy cache = cache_policy::declared;
        std::optional<artifact_manifest> output;
        std::vector<receipt_input> inputs;
        std::map<std::string, std::string> metadata;
        std::string handle;
        std::uint64_t elapsed_ms = 0;
        int exit_code = 0;
    };

    enum class visit { unseen, active, done };

    void configure_paths(const run_options& selected);
    int run_impl(run_options selected);
    bool prepare_state(bool create);
    void load(const std::filesystem::path& file);
    [[noreturn]] void parse_error(std::size_t line, const std::string& message) const;
    std::filesystem::path resolve(const std::string& path) const;
    std::filesystem::path resolve_output(const std::string& path) const;
    void ensure_profiles();
    executor external_executor(const std::string& profile_name);
    void validate_and_schedule(const std::vector<std::string>& targets);
    std::size_t schedule_name(const std::string& name,
                              std::map<std::string, visit, std::less<>>& marks,
                              std::vector<std::string> stack);
    int evaluate();
    built build_node(const std::string& name);
    built execute_rule(const rule& selected, const std::vector<built>& dependencies);

    std::vector<std::string> mapped_inputs(const rule& selected,
                                           const std::vector<built>& dependencies) const;
    std::vector<command_spec> expand_commands(const rule& selected,
                                              const std::vector<std::string>& inputs,
                                              const std::string& output,
                                              bool compatibility_shell) const;
    std::vector<std::pair<std::string, std::string>> environment_fingerprint() const;
    std::string command_digest(const std::vector<command_spec>& commands) const;
    std::string task_identity(const rule& selected, const executor& implementation,
                              const invocation& call, const std::vector<built>& dependencies,
                              std::string_view executor_fingerprint) const;

    std::filesystem::path receipt_path(std::string_view task_key) const;
    std::filesystem::path head_path(std::string_view target) const;
    receipt_data read_receipt(std::string_view task_key) const;
    void write_receipt(const receipt_data& receipt) const;
    std::optional<receipt_data> previous_receipt(std::string_view target) const;
    void write_head(std::string_view target, std::string_view task_key) const;
    std::vector<std::string> explain_changes(const receipt_data* previous,
                                             const rule& selected,
                                             const invocation& call,
                                             const std::vector<built>& dependencies,
                                             std::string_view executor_identity) const;
    void write_attempt(const receipt_data& receipt, std::string_view status,
                       std::string_view message = {}) const;

    int inspect_target(const std::string& target);
    int list_targets();
    int cache_command(operation selected);
    std::set<std::string> referenced_blobs() const;
    bool verify_receipt_artifacts(const receipt_data& receipt) const;

    void emit(event value) const;
    static std::string_view event_name(event_kind selected);
    static void render_text(const event& selected);
    static void render_json(const event& selected);

    static std::string record_value(const std::map<std::string, std::string>& record,
                                    const std::string& key);
    static std::uint64_t parse_unsigned(std::string_view value, std::string_view field);
    static int parse_integer(std::string_view value, std::string_view field);
    static artifact_manifest parse_manifest(const std::map<std::string, std::string>& record,
                                            std::string_view prefix);
    static void append_manifest(std::vector<std::pair<std::string, std::string>>& fields,
                                std::string_view prefix, const artifact_manifest& manifest);
    static std::string manifest_identity(const artifact_manifest& manifest);

    std::map<std::string, executor, std::less<>> profiles_;
    std::map<std::string, rule, std::less<>> rules_;
    std::vector<std::string> order_;
    std::map<std::string, built, std::less<>> results_;
    std::map<std::size_t, std::vector<std::string>> levels_;
    std::map<std::string, std::size_t, std::less<>> depths_;
    std::filesystem::path root_;
    std::filesystem::path state_root_;
    run_options current_;
    std::string run_id_;
    mutable std::mutex event_mutex_;
};

inline engine::engine() {
    profile("local", local_executor("local-argv-v2"));
    profile("shell", shell_executor("shell-compat-v2"));
}

inline void engine::profile(std::string profile_name, executor implementation) {
    if (profile_name.empty() || profile_name.find_first_of(" \t\r\n@/") != std::string::npos)
        throw error("Invalid executor profile name: " + profile_name);
    if (implementation.cache_identity.empty() || !implementation.run)
        throw error("Executor profile is incomplete: " + profile_name);
    profiles_[std::move(profile_name)] = std::move(implementation);
}

inline std::string_view engine::event_name(event_kind selected) {
    switch (selected) {
    case event_kind::plan: return "plan";
    case event_kind::current: return "current";
    case event_kind::run: return "run";
    case event_kind::deferred: return "deferred";
    case event_kind::restored: return "restored";
    case event_kind::done: return "done";
    case event_kind::target: return "target";
    case event_kind::inspect: return "inspect";
    case event_kind::cache: return "cache";
    case event_kind::warning: return "warning";
    case event_kind::log: return "log";
    case event_kind::failure: return "failure";
    }
    return "event";
}

inline void engine::render_text(const event& selected) {
    if (selected.kind == event_kind::warning || selected.kind == event_kind::failure ||
        selected.kind == event_kind::log) {
        if (!selected.target.empty())
            std::cerr << selected.target << ": ";
        if (selected.kind == event_kind::warning)
            std::cerr << "warning: ";
        std::cerr << selected.message << '\n';
        return;
    }
    std::cout << std::left << std::setw(10) << event_name(selected.kind) << selected.target;
    if (!selected.profile.empty())
        std::cout << "   @" << selected.profile;
    for (const auto& fact : selected.facts)
        std::cout << "  " << fact.first << '=' << fact.second;
    if (!selected.message.empty())
        std::cout << "  " << selected.message;
    if (selected.elapsed.count() > 0)
        std::cout << "  " << std::fixed << std::setprecision(3)
                  << static_cast<double>(selected.elapsed.count()) / 1000.0 << 's';
    if (selected.artifact)
        std::cout << "  sha256:" << selected.artifact->digest.substr(0, 12);
    std::cout << '\n';
}

inline void engine::render_json(const event& selected) {
    detail::json::object encoded{{"event", std::string(event_name(selected.kind))},
                                 {"message", selected.message},
                                 {"profile", selected.profile},
                                 {"target", selected.target}};
    detail::json::object facts;
    for (const auto& fact : selected.facts)
        facts.emplace(fact.first, fact.second);
    encoded.emplace("facts", std::move(facts));
    encoded.emplace("elapsed_ms", std::to_string(selected.elapsed.count()));
    if (selected.artifact)
        encoded.emplace("artifact", detail::artifact_json(*selected.artifact));
    std::cout << detail::json_dump(detail::json(std::move(encoded))) << '\n';
}

inline void engine::emit(event value) const {
    std::lock_guard<std::mutex> lock(event_mutex_);
    if (current_.events)
        current_.events(value);
}

inline void engine::help(std::ostream& out) {
    out << "usage: 0mk [OPTIONS] [run] [TARGET ...]\n"
           "       0mk [OPTIONS] plan [TARGET ...]\n"
           "       0mk [OPTIONS] why TARGET\n"
           "       0mk [OPTIONS] inspect TARGET\n"
           "       0mk [OPTIONS] targets\n"
           "       0mk [OPTIONS] cache verify|du|gc\n"
           "\n"
           "  -f FILE             read FILE instead of 0mkfile\n"
           "  -C DIR              operate relative to DIR\n"
           "  -n, --dry-run       alias for plan\n"
           "  -B, --force         execute even when an artifact is current\n"
           "      --why           explain decisions while running\n"
           "      --cache POLICY  off, declared (default), or hermetic\n"
           "      --env NAME      include an environment variable in task identity\n"
           "      --state-dir DIR place cache and receipts in DIR\n"
           "      --submit        approve executor plans that incur remote work\n"
           "  -j N                execute up to N independent tasks concurrently\n"
           "  -k, --keep-going    continue independent work after a failure\n"
           "      --json          emit JSONL events\n"
           "      --version       print the version\n";
}

inline int engine::cli(int argc, char** argv, std::filesystem::path default_file) {
    run_options selected;
    selected.file = std::move(default_file);
    bool command_selected = false;
    bool options_enabled = true;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (options_enabled && argument == "--") {
            options_enabled = false;
        } else if (options_enabled && argument == "-f") {
            if (++i == argc)
                throw error("-f requires a path");
            selected.file = argv[i];
        } else if (options_enabled && argument == "-C") {
            if (++i == argc)
                throw error("-C requires a directory");
            selected.directory = argv[i];
        } else if (options_enabled && argument == "--state-dir") {
            if (++i == argc)
                throw error("--state-dir requires a directory");
            selected.state_dir = argv[i];
        } else if (options_enabled && detail::starts_with(argument, "--state-dir=")) {
            selected.state_dir = argument.substr(std::string("--state-dir=").size());
        } else if (options_enabled && (argument == "-n" || argument == "--dry-run")) {
            selected.command = operation::plan;
            selected.dry_run = true;
            command_selected = true;
        } else if (options_enabled && (argument == "-B" || argument == "--force")) {
            selected.force = true;
        } else if (options_enabled && argument == "--why") {
            selected.why = true;
        } else if (options_enabled && argument == "--submit") {
            selected.submit = true;
        } else if (options_enabled && argument == "--json") {
            selected.json = true;
        } else if (options_enabled && (argument == "-k" || argument == "--keep-going")) {
            selected.keep_going = true;
        } else if (options_enabled && argument == "--cache") {
            if (++i == argc)
                throw error("--cache requires off, declared, or hermetic");
            selected.cache = parse_cache_policy(argv[i]);
        } else if (options_enabled && detail::starts_with(argument, "--cache=")) {
            selected.cache = parse_cache_policy(argument.substr(std::string("--cache=").size()));
        } else if (options_enabled && argument == "--env") {
            if (++i == argc)
                throw error("--env requires a variable name");
            selected.environment.emplace_back(argv[i]);
        } else if (options_enabled && argument == "-j") {
            if (++i == argc)
                throw error("-j requires a positive integer");
            selected.jobs = static_cast<std::size_t>(parse_unsigned(argv[i], "jobs"));
            if (!selected.jobs)
                throw error("-j requires a positive integer");
        } else if (options_enabled && argument.size() > 2 && argument[0] == '-' &&
                   argument[1] == 'j') {
            selected.jobs = static_cast<std::size_t>(parse_unsigned(argument.substr(2), "jobs"));
            if (!selected.jobs)
                throw error("-j requires a positive integer");
        } else if (options_enabled && argument == "--version") {
            std::cout << "0mk " << version << '\n';
            return static_cast<int>(exit_status::success);
        } else if (options_enabled && (argument == "-h" || argument == "--help")) {
            help(std::cout);
            return static_cast<int>(exit_status::success);
        } else if (options_enabled && !argument.empty() && argument.front() == '-') {
            throw error("Unknown option: " + argument);
        } else if (!command_selected && selected.targets.empty() && argument == "run") {
            selected.command = operation::run;
            command_selected = true;
        } else if (!command_selected && selected.targets.empty() && argument == "plan") {
            selected.command = operation::plan;
            selected.dry_run = true;
            command_selected = true;
        } else if (!command_selected && selected.targets.empty() && argument == "why") {
            selected.command = operation::why;
            selected.dry_run = true;
            selected.why = true;
            command_selected = true;
        } else if (!command_selected && selected.targets.empty() && argument == "inspect") {
            selected.command = operation::inspect;
            command_selected = true;
        } else if (!command_selected && selected.targets.empty() && argument == "targets") {
            selected.command = operation::targets;
            command_selected = true;
        } else if (!command_selected && selected.targets.empty() && argument == "cache") {
            if (++i == argc)
                throw error("cache requires verify, du, or gc");
            const std::string subcommand(argv[i]);
            if (subcommand == "verify")
                selected.command = operation::cache_verify;
            else if (subcommand == "du")
                selected.command = operation::cache_du;
            else if (subcommand == "gc")
                selected.command = operation::cache_gc;
            else
                throw error("Unknown cache command: " + subcommand);
            command_selected = true;
        } else {
            selected.targets.push_back(argument);
        }
    }
    if ((selected.command == operation::why || selected.command == operation::inspect) &&
        selected.targets.size() != 1)
        throw error(std::string(selected.command == operation::why ? "why" : "inspect") +
                    " requires exactly one target");
    if ((selected.command == operation::targets || selected.command == operation::cache_verify ||
         selected.command == operation::cache_du || selected.command == operation::cache_gc) &&
        !selected.targets.empty())
        throw error("This command does not accept targets");
    selected.events = selected.json ? event_sink([](const event& value) { render_json(value); })
                                    : event_sink([](const event& value) { render_text(value); });
    return run(std::move(selected));
}

inline std::string engine::record_value(const std::map<std::string, std::string>& record,
                                        const std::string& key) {
    const auto found = record.find(key);
    return found == record.end() ? std::string{} : found->second;
}

inline std::uint64_t engine::parse_unsigned(std::string_view value, std::string_view field) {
    if (value.empty())
        throw error("Invalid " + std::string(field));
    std::uint64_t result = 0;
    for (const char c : value) {
        if (c < '0' || c > '9')
            throw error("Invalid " + std::string(field));
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            throw error("Out-of-range " + std::string(field));
        result = result * 10 + digit;
    }
    return result;
}

inline int engine::parse_integer(std::string_view value, std::string_view field) {
    bool negative = false;
    if (!value.empty() && value.front() == '-') {
        negative = true;
        value.remove_prefix(1);
    }
    const auto magnitude = parse_unsigned(value, field);
    const auto limit = negative ? static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1U
                                : static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    if (magnitude > limit)
        throw error("Out-of-range " + std::string(field));
    if (negative && magnitude == limit)
        return std::numeric_limits<int>::min();
    const int converted = static_cast<int>(magnitude);
    return negative ? -converted : converted;
}

inline artifact_manifest engine::parse_manifest(
    const std::map<std::string, std::string>& record, std::string_view prefix) {
    const std::string base(prefix);
    const std::string kind = record_value(record, base + "KIND");
    artifact_manifest manifest;
    if (kind == "file")
        manifest.kind = artifact_kind::file;
    else if (kind == "tree")
        manifest.kind = artifact_kind::tree;
    else
        throw error("Invalid receipt artifact kind");
    manifest.digest = record_value(record, base + "DIGEST");
    if (!detail::valid_digest(manifest.digest))
        throw error("Invalid receipt artifact digest");
    manifest.size = parse_unsigned(record_value(record, base + "SIZE"), "artifact size");
    const auto mode = parse_unsigned(record_value(record, base + "MODE"), "artifact mode");
    if (mode > 07777U)
        throw error("Invalid receipt artifact mode");
    manifest.mode = static_cast<std::uint32_t>(mode);
    return manifest;
}

inline void engine::append_manifest(
    std::vector<std::pair<std::string, std::string>>& fields, std::string_view prefix,
    const artifact_manifest& manifest) {
    const std::string base(prefix);
    fields.emplace_back(base + "KIND", std::string(name(manifest.kind)));
    fields.emplace_back(base + "DIGEST", manifest.digest);
    fields.emplace_back(base + "SIZE", std::to_string(manifest.size));
    fields.emplace_back(base + "MODE", std::to_string(manifest.mode));
}

inline std::string engine::manifest_identity(const artifact_manifest& manifest) {
    detail::fingerprint identity;
    identity.add(std::string(name(manifest.kind)));
    identity.add(manifest.digest);
    identity.add(std::to_string(manifest.size));
    identity.add(std::to_string(manifest.mode));
    return identity.finish();
}

inline void engine::configure_paths(const run_options& selected) {
    std::filesystem::path base = selected.directory.empty() ? std::filesystem::current_path()
                                                             : selected.directory;
    if (!base.is_absolute())
        base = std::filesystem::absolute(base);
    const auto file = selected.file.is_absolute() ? selected.file : base / selected.file;
    root_ = std::filesystem::absolute(file).parent_path().lexically_normal();
    if (selected.state_dir.empty()) {
        state_root_ = root_ / ".0mk";
    } else {
        state_root_ = selected.state_dir.is_absolute() ? selected.state_dir
                                                       : root_ / selected.state_dir;
        state_root_ = std::filesystem::absolute(state_root_).lexically_normal();
    }
}

inline bool engine::prepare_state(bool create) {
    std::error_code root_error;
    std::error_code state_error;
    const auto canonical_root = std::filesystem::weakly_canonical(root_, root_error);
    const auto canonical_state = std::filesystem::weakly_canonical(state_root_, state_error);
    if (root_error || state_error)
        throw error("Cannot resolve the project or state directory");
    if (detail::path_has_prefix(canonical_root, canonical_state))
        throw error("State directory may not be the project directory or one of its ancestors: " +
                    state_root_.string());

    const auto status = std::filesystem::symlink_status(state_root_);
    if (std::filesystem::is_symlink(status))
        throw error("State directory may not be a symlink: " + state_root_.string());
    if (!std::filesystem::exists(status)) {
        if (!create)
            return false;
        std::filesystem::create_directories(state_root_);
    } else if (!std::filesystem::is_directory(status)) {
        throw error("State path is not a directory: " + state_root_.string());
    }

    const auto marker = state_root_ / ".0mk-state";
    const auto marker_status = std::filesystem::symlink_status(marker);
    const auto validate_layout = [&] {
        static constexpr std::array<std::string_view, 8> directories{
            "objects", "trees", "receipts", "targets", "heads", "runs", "work", "locks"};
        for (const auto name_value : directories) {
            const auto path = state_root_ / std::string(name_value);
            const auto path_status = std::filesystem::symlink_status(path);
            if (std::filesystem::exists(path_status) &&
                (std::filesystem::is_symlink(path_status) ||
                 !std::filesystem::is_directory(path_status)))
                throw error("Unsafe 0mk state namespace: " + path.string());
        }
        const auto lock = state_root_ / "cache.lock";
        const auto lock_status = std::filesystem::symlink_status(lock);
        if (std::filesystem::exists(lock_status) &&
            (std::filesystem::is_symlink(lock_status) ||
             !std::filesystem::is_regular_file(lock_status)))
            throw error("Unsafe 0mk state lock: " + lock.string());
    };
    if (std::filesystem::exists(marker_status)) {
        if (std::filesystem::is_symlink(marker_status) ||
            !std::filesystem::is_regular_file(marker_status))
            throw error("Invalid 0mk state marker: " + marker.string());
        std::ifstream input(marker);
        std::string format;
        std::string extra;
        if (!std::getline(input, format) || format != "0MK_STATE_V1" ||
            std::getline(input, extra))
            throw error("Unsupported 0mk state format: " + marker.string());
        validate_layout();
        return true;
    }

    if (std::filesystem::directory_iterator(state_root_) !=
        std::filesystem::directory_iterator{})
        throw error("Refusing to use an unmarked nonempty state directory: " +
                    state_root_.string());
    if (!create)
        return false;
    detail::write_atomically(marker, "0MK_STATE_V1\n", 0444);
    validate_layout();
    return true;
}

inline std::filesystem::path engine::resolve(const std::string& path) const {
    const std::filesystem::path value(path);
    return value.is_absolute() ? value : std::filesystem::absolute(root_ / value).lexically_normal();
}

inline std::filesystem::path engine::resolve_output(const std::string& path) const {
    const auto output = resolve(path);
    const auto relative = output.lexically_relative(root_);
    std::filesystem::path parent = root_;
    for (const auto& component : relative.parent_path()) {
        parent /= component;
        const auto status = std::filesystem::symlink_status(parent);
        if (std::filesystem::is_symlink(status))
            throw error("Artifact target parent may not be a symlink: " + parent.string());
        if (std::filesystem::exists(status) && !std::filesystem::is_directory(status))
            throw error("Artifact target parent is not a directory: " + parent.string());
    }
    return output;
}

inline void engine::parse_error(std::size_t line, const std::string& message) const {
    throw error(current_.file.string() + ':' + std::to_string(line) + ": " + message);
}

inline void engine::load(const std::filesystem::path& file) {
    rules_.clear();
    order_.clear();
    results_.clear();
    levels_.clear();
    depths_.clear();

    std::filesystem::path base = current_.directory.empty() ? std::filesystem::current_path()
                                                             : current_.directory;
    if (!base.is_absolute())
        base = std::filesystem::absolute(base);
    const auto absolute = std::filesystem::absolute(file.is_absolute() ? file : base / file)
                              .lexically_normal();
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
            try {
                if (detail::words(cleaned).empty())
                    parse_error(line_number, "empty recipe");
            } catch (const std::exception& failure) {
                parse_error(line_number, failure.what());
            }
            current->recipes.push_back(cleaned);
            continue;
        }

        const auto arrow = line.find("<-");
        if (arrow == std::string::npos)
            parse_error(line_number, "expected '<-'");
        std::vector<std::string> targets;
        try {
            targets = detail::words(detail::trim(line.substr(0, arrow)));
        } catch (const std::exception& failure) {
            parse_error(line_number, failure.what());
        }
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
            parse_error(line_number, "target must name an artifact or action");
        if (!target_path.empty() && *target_path.begin() == ".0mk")
            parse_error(line_number, "targets may not use the reserved .0mk directory");
        if (target_path.generic_string() != parsed.target)
            parse_error(line_number, "target path is not normalized; use '" +
                                         target_path.generic_string() + "'");
        if (detail::path_has_prefix(resolve_output(parsed.target), state_root_))
            parse_error(line_number, "targets may not use the configured state directory");

        std::vector<std::string> right;
        try {
            right = detail::words(line.substr(arrow + 2));
        } catch (const std::exception& failure) {
            parse_error(line_number, failure.what());
        }
        if (!right.empty() && !right.back().empty() && right.back().front() == '@') {
            parsed.profile = right.back().substr(1);
            right.pop_back();
            if (parsed.profile.empty())
                parse_error(line_number, "empty executor profile");
        }
        for (const auto& item : right)
            if (!item.empty() && item.front() == '@')
                parse_error(line_number, "executor profile must be the final token");
        for (auto& item : right) {
            item = detail::normalize_reference(std::move(item));
            if (detail::path_has_prefix(resolve(item), state_root_))
                parse_error(line_number, "dependencies may not use the configured state directory");
        }
        parsed.dependencies = std::move(right);

        if (rules_.count(parsed.target))
            parse_error(line_number, "duplicate target: " + parsed.target);
        order_.push_back(parsed.target);
        auto inserted = rules_.emplace(parsed.target, std::move(parsed));
        current = &inserted.first->second;
    }
    if (order_.empty())
        throw error("0mkfile contains no rules");

    for (const auto& item : rules_) {
        const rule& selected = item.second;
        if (selected.action && selected.recipes.empty())
            parse_error(selected.line, "an action requires a recipe");
        if (selected.recipes.empty() && selected.profile != "local")
            parse_error(selected.line, "an alias may not select an executor profile");
        bool uses_output = false;
        for (const auto& recipe : selected.recipes) {
            std::vector<std::string> tokens;
            try {
                tokens = detail::words(recipe);
            } catch (const std::exception& failure) {
                parse_error(selected.line, failure.what());
            }
            for (std::string token : tokens) {
                detail::replace_all(token, "$$", "\x1f");
                uses_output = uses_output || token.find("$@") != std::string::npos;
                if (token.find("$^") != std::string::npos && token != "$^")
                    parse_error(selected.line, "$^ must occupy a complete recipe argument");
            }
        }
        if (!selected.action && !selected.recipes.empty() && !uses_output)
            parse_error(selected.line, "an artifact recipe must write $@");
    }
    for (auto left = rules_.begin(); left != rules_.end(); ++left) {
        if (left->second.action || left->second.recipes.empty())
            continue;
        for (auto right = std::next(left); right != rules_.end(); ++right) {
            if (right->second.action || right->second.recipes.empty())
                continue;
            const std::filesystem::path left_path(left->first);
            const std::filesystem::path right_path(right->first);
            if (detail::path_has_prefix(left_path, right_path) ||
                detail::path_has_prefix(right_path, left_path))
                parse_error(right->second.line, "artifact targets may not overlap: " +
                                                    left->first + " and " + right->first);
        }
    }
    for (const auto& item : rules_) {
        const rule& selected = item.second;
        if (selected.recipes.empty())
            continue;
        std::vector<std::pair<std::string, std::filesystem::path>> inputs;
        for (std::size_t i = 0; i < selected.dependencies.size(); ++i) {
            const std::string& dependency = selected.dependencies[i];
            const auto dependency_rule = rules_.find(dependency);
            if (dependency_rule != rules_.end() && dependency_rule->second.recipes.empty())
                continue;
            const std::filesystem::path raw(dependency);
            bool safe = !raw.empty() && !raw.is_absolute() && raw != "." &&
                        raw.lexically_normal() == raw;
            for (const auto& component : raw)
                if (component == "..")
                    safe = false;
            const auto mapped = safe && *raw.begin() != ".0mk"
                                    ? raw
                                    : std::filesystem::path(".inputs") / std::to_string(i) /
                                          (raw.filename().empty() ? "input" : raw.filename());
            inputs.emplace_back(dependency, mapped);
        }
        const std::filesystem::path target(selected.target);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (!selected.action &&
                (detail::path_has_prefix(target, inputs[i].second) ||
                 detail::path_has_prefix(inputs[i].second, target)))
                parse_error(selected.line, "artifact target and input may not overlap: " +
                                               selected.target + " and " + inputs[i].first);
            for (std::size_t j = i + 1; j < inputs.size(); ++j)
                if (detail::path_has_prefix(inputs[i].second, inputs[j].second) ||
                    detail::path_has_prefix(inputs[j].second, inputs[i].second))
                    parse_error(selected.line, "artifact inputs may not overlap: " +
                                                   inputs[i].first + " and " + inputs[j].first);
        }
    }
}

inline executor engine::external_executor(const std::string& profile_name) {
    const std::string command = "0mk-exec-" + profile_name;
    const auto executable_path = detail::find_executable(command, root_);
    if (executable_path.empty())
        throw error("Unknown executor profile '@" + profile_name +
                    "' (expected " + command + " on PATH)");
    const std::string request = detail::protocol_request("identity", profile_name);
    const auto response = detail::request_process({executable_path.string(), "identity"}, root_, request);
    const auto object = detail::protocol_response(response, "identity", profile_name);
    const auto* identity_value = detail::json_find(object, "identity");
    const auto* cache_value = detail::json_find(object, "cache_policy");
    if (!identity_value || !cache_value)
        throw adapter_failure("Executor '" + profile_name +
                              "' returned an incomplete identity");

    executor implementation;
    implementation.external = true;
    implementation.executable = executable_path.string();
    std::string reported_identity;
    try {
        reported_identity = identity_value->as_string();
        implementation.guarantee = parse_cache_policy(cache_value->as_string());
    } catch (const std::exception& failure) {
        throw adapter_failure("Executor '" + profile_name +
                              "' returned an invalid identity: " + failure.what());
    }
    detail::fingerprint identity;
    identity.add("external-executor-v1");
    identity.add(reported_identity);
    identity.add(detail::hash_file(executable_path));
    identity.add(std::to_string(detail::path_mode(executable_path)));
    implementation.cache_identity = identity.finish();
    implementation.fingerprint = [path = implementation.executable, profile_name,
                                  expected_policy = implementation.guarantee](
                                     const invocation& call) {
        const auto refreshed = detail::request_process(
            {path, "identity"}, call.source_root,
            detail::protocol_request("identity", profile_name));
        const auto refreshed_object =
            detail::protocol_response(refreshed, "identity", profile_name);
        const auto* refreshed_identity = detail::json_find(refreshed_object, "identity");
        const auto* refreshed_policy = detail::json_find(refreshed_object, "cache_policy");
        if (!refreshed_identity || !refreshed_policy ||
            parse_cache_policy(refreshed_policy->as_string()) != expected_policy)
            throw error("Executor '" + profile_name + "' changed its cache identity contract");
        detail::fingerprint value;
        value.add("external-executor-v1");
        value.add(refreshed_identity->as_string());
        value.add(detail::hash_file(path));
        value.add(std::to_string(detail::path_mode(path)));
        return value.finish();
    };
    implementation.plan = [path = implementation.executable, profile_name](const invocation& call) {
        const auto result = detail::request_process(
            {path, "plan"}, call.source_root,
            detail::protocol_request("plan", profile_name, &call));
        const auto response_object = detail::protocol_response(result, "plan", profile_name);
        mk0::plan planned;
        planned.facts = detail::json_string_map(detail::json_find(response_object, "facts"));
        planned.warnings = detail::json_string_array(detail::json_find(response_object, "warnings"));
        if (const auto* required = detail::json_find(response_object, "requires_submit"))
            planned.requires_submit = required->as_bool();
        if (const auto* cost = detail::json_find(response_object, "estimated_cost_usd"))
            planned.estimated_cost_usd = cost->as_number();
        return planned;
    };
    implementation.run = [path = implementation.executable, profile_name](const invocation& call,
                                                                           log_sink log) {
        log("executor " + profile_name + " run");
        const auto result = detail::request_process(
            {path, "run"}, call.source_root,
            detail::protocol_request("run", profile_name, &call));
        const auto response_object = detail::protocol_response(result, "run", profile_name);
        const auto* exit = detail::json_find(response_object, "exit_code");
        if (!exit)
            throw error("Executor '" + profile_name + "' omitted exit_code");
        const double raw_exit = exit->as_number();
        if (!std::isfinite(raw_exit) ||
            raw_exit < static_cast<double>(std::numeric_limits<int>::min()) ||
            raw_exit > static_cast<double>(std::numeric_limits<int>::max()) ||
            raw_exit != static_cast<double>(static_cast<int>(raw_exit)))
            throw error("Executor '" + profile_name + "' returned an invalid exit_code");
        mk0::result outcome;
        outcome.exit_code = static_cast<int>(raw_exit);
        outcome.metadata = detail::json_string_map(detail::json_find(response_object, "metadata"));
        if (const auto* handle = detail::json_find(response_object, "handle"))
            outcome.handle = handle->as_string();
        return outcome;
    };
    return implementation;
}

inline void engine::ensure_profiles() {
    std::set<std::string> requested;
    for (const auto& item : rules_)
        if (!item.second.recipes.empty() && !profiles_.count(item.second.profile))
            requested.insert(item.second.profile);
    for (const auto& profile_name : requested)
        profile(profile_name, external_executor(profile_name));
}

inline std::size_t engine::schedule_name(
    const std::string& name, std::map<std::string, visit, std::less<>>& marks,
    std::vector<std::string> stack) {
    if (marks[name] == visit::done)
        return depths_.at(name);
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

    const auto found = rules_.find(name);
    if (found == rules_.end()) {
        const auto path = resolve(name);
        const auto status = std::filesystem::symlink_status(path);
        if (std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)))
            throw error("No rule to make target '" + name + "'");
        marks[name] = visit::done;
        depths_[name] = 0;
        levels_[0].push_back(name);
        return 0;
    }

    const rule& selected = found->second;
    if (!selected.recipes.empty() && !profiles_.count(selected.profile))
        throw error("Unknown executor profile '@" + selected.profile + "' for " + name);
    marks[name] = visit::active;
    stack.push_back(name);
    std::size_t depth = 0;
    for (const auto& dependency : selected.dependencies) {
        const auto dependency_rule = rules_.find(dependency);
        if (dependency_rule != rules_.end() && dependency_rule->second.action)
            throw error("Action '" + dependency + "' may not be a dependency of '" + name + "'");
        depth = std::max(depth, schedule_name(dependency, marks, stack) + 1);
    }
    marks[name] = visit::done;
    depths_[name] = depth;
    levels_[depth].push_back(name);
    return depth;
}

inline void engine::validate_and_schedule(const std::vector<std::string>& targets) {
    levels_.clear();
    depths_.clear();
    std::map<std::string, visit, std::less<>> marks;
    for (const auto& target : targets)
        (void)schedule_name(target, marks, {});
}

inline std::vector<std::string> engine::mapped_inputs(
    const rule& selected, const std::vector<built>& dependencies) const {
    std::vector<std::string> mapped;
    for (std::size_t i = 0; i < dependencies.size(); ++i) {
        if (!dependencies[i].artifact)
            continue;
        const std::filesystem::path raw(selected.dependencies[i]);
        bool safe = !raw.empty() && !raw.is_absolute() && raw != "." &&
                    raw.lexically_normal() == raw;
        for (const auto& component : raw)
            if (component == "..")
                safe = false;
        if (safe && *raw.begin() != ".0mk") {
            mapped.push_back(raw.generic_string());
        } else {
            const std::string filename = raw.filename().empty() ? "input" : raw.filename().string();
            mapped.push_back((std::filesystem::path(".inputs") / std::to_string(i) / filename)
                                 .generic_string());
        }
    }
    return mapped;
}

inline std::vector<command_spec> engine::expand_commands(
    const rule& selected, const std::vector<std::string>& inputs, const std::string& output,
    bool compatibility_shell) const {
    std::vector<command_spec> commands;
    const std::string first = inputs.empty() ? std::string{} : inputs.front();
    std::string shell_all;
    for (const auto& input : inputs) {
        if (!shell_all.empty())
            shell_all.push_back(' ');
        shell_all += detail::shell_quote(input);
    }
    for (const auto& recipe : selected.recipes) {
        command_spec command;
        command.raw = recipe;
        if (compatibility_shell) {
            command.kind = command_kind::shell;
            command.shell = detail::expand_shell(recipe, detail::shell_quote(output),
                                                 detail::shell_quote(first), shell_all);
        } else {
            command.kind = command_kind::argv;
            for (std::string token : detail::words(recipe)) {
                if (token == "$^") {
                    command.argv.insert(command.argv.end(), inputs.begin(), inputs.end());
                    continue;
                }
                constexpr std::string_view sentinel = "\x1fMK0_DOLLAR\x1f";
                detail::replace_all(token, "$$", sentinel);
                detail::replace_all(token, "$@", output);
                detail::replace_all(token, "$<", first);
                detail::replace_all(token, sentinel, "$");
                command.argv.push_back(std::move(token));
            }
        }
        commands.push_back(std::move(command));
    }
    return commands;
}

inline std::vector<std::pair<std::string, std::string>> engine::environment_fingerprint() const {
    std::set<std::string> names(current_.environment.begin(), current_.environment.end());
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& selected : names) {
        if (selected.empty() || selected.find('=') != std::string::npos)
            throw error("Invalid environment variable name: " + selected);
        const char* value = std::getenv(selected.c_str());
        detail::fingerprint digest;
        digest.add(value ? "present" : "missing");
        if (value)
            digest.add(value);
        result.emplace_back(selected, digest.finish());
    }
    return result;
}

inline std::string engine::command_digest(const std::vector<command_spec>& commands) const {
    detail::fingerprint digest;
    digest.add("0mk-command-list-v1");
    for (const auto& command : commands) {
        digest.add(command.kind == command_kind::argv ? "argv" : "shell");
        digest.add(command.raw);
        if (command.kind == command_kind::argv)
            for (const auto& argument : command.argv)
                digest.add(argument);
        if (command.kind == command_kind::shell)
            digest.add(command.shell);
    }
    return digest.finish();
}

inline std::string engine::task_identity(
    const rule& selected, const executor& implementation, const invocation& call,
    const std::vector<built>& dependencies, std::string_view executor_fingerprint) const {
    detail::fingerprint identity;
    identity.add(task_abi);
    identity.add(selected.target);
    identity.add(std::string(name(call.cache)));
    identity.add(selected.profile);
    identity.add(implementation.cache_identity);
    identity.add(executor_fingerprint);
    identity.add(command_digest(call.commands));
    for (const auto& environment : call.environment) {
        identity.add(environment.first);
        identity.add(environment.second);
    }
    for (std::size_t i = 0; i < dependencies.size(); ++i) {
        identity.add(selected.dependencies[i]);
        identity.add(dependencies[i].identity);
        if (dependencies[i].artifact)
            identity.add(manifest_identity(*dependencies[i].artifact));
        else
            identity.add("no-artifact");
    }
    identity.add("materialized-inputs");
    for (const auto& input : call.inputs) {
        identity.add(input.logical_name);
        identity.add(manifest_identity(input.artifact));
    }
    return identity.finish();
}

inline std::filesystem::path engine::receipt_path(std::string_view task_key) const {
    if (!detail::valid_digest(task_key))
        throw error("Invalid task identity");
    return state_root_ / "receipts" / (std::string(task_key) + ".txt");
}

inline std::filesystem::path engine::head_path(std::string_view target) const {
    return state_root_ / "targets" / (detail::hash_text(target) + ".txt");
}

inline engine::receipt_data engine::read_receipt(std::string_view task_key) const {
    receipt_data receipt;
    if (!detail::valid_digest(task_key))
        return receipt;
    const auto record = detail::read_record(receipt_path(task_key));
    if (record.empty() || record_value(record, "VERSION") != "2")
        return receipt;
    try {
        receipt.task_key = record_value(record, "TASK_KEY");
        receipt.target = detail::hex_decode(record_value(record, "TARGET_HEX"));
        receipt.profile = detail::hex_decode(record_value(record, "PROFILE_HEX"));
        receipt.executor_identity = record_value(record, "EXECUTOR_IDENTITY");
        receipt.canonical_command = detail::hex_decode(record_value(record, "COMMAND_HEX"));
        receipt.command_digest = record_value(record, "COMMAND_DIGEST");
        receipt.environment_digest = record_value(record, "ENVIRONMENT_DIGEST");
        receipt.cache = parse_cache_policy(record_value(record, "CACHE_POLICY"));
        receipt.elapsed_ms = parse_unsigned(record_value(record, "ELAPSED_MS"), "elapsed time");
        receipt.exit_code = parse_integer(record_value(record, "EXIT_CODE"), "exit code");
        receipt.handle = detail::hex_decode(record_value(record, "HANDLE_HEX"));
        if (receipt.task_key != task_key || !detail::valid_digest(receipt.task_key) ||
            !detail::valid_digest(receipt.executor_identity) ||
            !detail::valid_digest(receipt.command_digest) ||
            !detail::valid_digest(receipt.environment_digest))
            return receipt_data{};
        if (record_value(record, "OUTPUT_PRESENT") == "1")
            receipt.output = parse_manifest(record, "OUTPUT_");
        else if (record_value(record, "OUTPUT_PRESENT") != "0")
            return receipt_data{};
        const auto input_count = parse_unsigned(record_value(record, "INPUT_COUNT"), "input count");
        if (input_count > 100000)
            return receipt_data{};
        for (std::uint64_t i = 0; i < input_count; ++i) {
            const std::string prefix = "INPUT_" + std::to_string(i) + "_";
            receipt_input input;
            input.name = detail::hex_decode(record_value(record, prefix + "NAME_HEX"));
            input.identity = record_value(record, prefix + "IDENTITY");
            if (!detail::valid_digest(input.identity))
                return receipt_data{};
            const std::string present = record_value(record, prefix + "ARTIFACT_PRESENT");
            if (present == "1")
                input.artifact = parse_manifest(record, prefix + "ARTIFACT_");
            else if (present != "0")
                return receipt_data{};
            receipt.inputs.push_back(std::move(input));
        }
        const auto metadata_count = parse_unsigned(record_value(record, "METADATA_COUNT"),
                                                   "metadata count");
        if (metadata_count > 100000)
            return receipt_data{};
        for (std::uint64_t i = 0; i < metadata_count; ++i) {
            const std::string prefix = "METADATA_" + std::to_string(i) + "_";
            receipt.metadata.emplace(
                detail::hex_decode(record_value(record, prefix + "KEY_HEX")),
                detail::hex_decode(record_value(record, prefix + "VALUE_HEX")));
        }
        receipt.valid = true;
    } catch (...) {
        return receipt_data{};
    }
    return receipt;
}

inline void engine::write_receipt(const receipt_data& receipt) const {
    std::vector<std::pair<std::string, std::string>> fields{
        {"VERSION", "2"},
        {"TASK_KEY", receipt.task_key},
        {"TARGET_HEX", detail::hex_encode(receipt.target)},
        {"PROFILE_HEX", detail::hex_encode(receipt.profile)},
        {"EXECUTOR_IDENTITY", receipt.executor_identity},
        {"COMMAND_HEX", detail::hex_encode(receipt.canonical_command)},
        {"COMMAND_DIGEST", receipt.command_digest},
        {"ENVIRONMENT_DIGEST", receipt.environment_digest},
        {"CACHE_POLICY", std::string(name(receipt.cache))},
        {"ELAPSED_MS", std::to_string(receipt.elapsed_ms)},
        {"EXIT_CODE", std::to_string(receipt.exit_code)},
        {"HANDLE_HEX", detail::hex_encode(receipt.handle)},
        {"OUTPUT_PRESENT", receipt.output ? "1" : "0"}};
    if (receipt.output)
        append_manifest(fields, "OUTPUT_", *receipt.output);
    fields.emplace_back("INPUT_COUNT", std::to_string(receipt.inputs.size()));
    for (std::size_t i = 0; i < receipt.inputs.size(); ++i) {
        const std::string prefix = "INPUT_" + std::to_string(i) + "_";
        fields.emplace_back(prefix + "NAME_HEX", detail::hex_encode(receipt.inputs[i].name));
        fields.emplace_back(prefix + "IDENTITY", receipt.inputs[i].identity);
        fields.emplace_back(prefix + "ARTIFACT_PRESENT", receipt.inputs[i].artifact ? "1" : "0");
        if (receipt.inputs[i].artifact)
            append_manifest(fields, prefix + "ARTIFACT_", *receipt.inputs[i].artifact);
    }
    fields.emplace_back("METADATA_COUNT", std::to_string(receipt.metadata.size()));
    std::size_t metadata_index = 0;
    for (const auto& item : receipt.metadata) {
        const std::string prefix = "METADATA_" + std::to_string(metadata_index++) + "_";
        fields.emplace_back(prefix + "KEY_HEX", detail::hex_encode(item.first));
        fields.emplace_back(prefix + "VALUE_HEX", detail::hex_encode(item.second));
    }
    detail::write_record(receipt_path(receipt.task_key), fields);
}

inline std::optional<engine::receipt_data> engine::previous_receipt(
    std::string_view target) const {
    try {
        const auto head = detail::read_record(head_path(target));
        if (head.empty() || record_value(head, "VERSION") != "1" ||
            detail::hex_decode(record_value(head, "TARGET_HEX")) != target)
            return std::nullopt;
        receipt_data receipt = read_receipt(record_value(head, "TASK_KEY"));
        if (!receipt.valid || receipt.target != target)
            return std::nullopt;
        return receipt;
    } catch (...) {
        return std::nullopt;
    }
}

inline void engine::write_head(std::string_view target, std::string_view task_key) const {
    detail::write_record(head_path(target), {{"VERSION", "1"},
                                             {"TARGET_HEX", detail::hex_encode(target)},
                                             {"TASK_KEY", std::string(task_key)}});
}

inline std::vector<std::string> engine::explain_changes(
    const receipt_data* previous, const rule& selected, const invocation& call,
    const std::vector<built>& dependencies, std::string_view executor_identity) const {
    std::vector<std::string> reasons;
    if (!previous) {
        reasons.emplace_back("no previous task");
        return reasons;
    }
    if (previous->profile != selected.profile)
        reasons.emplace_back("executor profile changed");
    if (previous->cache != call.cache)
        reasons.emplace_back("cache policy changed from " +
                             std::string(name(previous->cache)) + " to " +
                             std::string(name(call.cache)));
    if (previous->executor_identity != executor_identity)
        reasons.emplace_back("executor identity changed");
    if (previous->command_digest != command_digest(call.commands))
        reasons.emplace_back("command changed");
    detail::fingerprint environment;
    for (const auto& item : call.environment) {
        environment.add(item.first);
        environment.add(item.second);
    }
    if (previous->environment_digest != environment.finish())
        reasons.emplace_back("declared environment changed");
    std::vector<receipt_input> current_inputs;
    current_inputs.reserve(dependencies.size() + call.inputs.size());
    for (std::size_t i = 0; i < dependencies.size(); ++i)
        current_inputs.push_back(
            {selected.dependencies[i], dependencies[i].identity, dependencies[i].artifact});
    for (const auto& input : call.inputs)
        if (detail::starts_with(input.logical_name, "tool:"))
            current_inputs.push_back(
                {input.logical_name, manifest_identity(input.artifact), input.artifact});
    for (const auto& current_input : current_inputs) {
        const auto found = std::find_if(previous->inputs.begin(), previous->inputs.end(),
                                        [&](const receipt_input& input) {
                                            return input.name == current_input.name;
                                        });
        if (found == previous->inputs.end()) {
            reasons.push_back(current_input.name + " was added");
        } else if (found->identity != current_input.identity) {
            const std::string before = found->artifact ? found->artifact->digest : found->identity;
            const std::string after = current_input.artifact ? current_input.artifact->digest
                                                              : current_input.identity;
            reasons.push_back(current_input.name + " changed (previous sha256:" + before +
                              ", current sha256:" + after + ")");
        }
    }
    for (const auto& input : previous->inputs)
        if (std::none_of(current_inputs.begin(), current_inputs.end(),
                         [&](const auto& current_input) {
                             return current_input.name == input.name;
                         }))
            reasons.push_back(input.name + " was removed");
    if (reasons.empty())
        reasons.emplace_back(current_.force ? "forced" : "cached artifact is unavailable");
    return reasons;
}

inline void engine::write_attempt(const receipt_data& receipt, std::string_view status,
                                  std::string_view message) const {
    if (run_id_.empty())
        return;
    std::vector<std::pair<std::string, std::string>> fields{
        {"VERSION", "1"},
        {"TARGET_HEX", detail::hex_encode(receipt.target)},
        {"TASK_KEY", receipt.task_key},
        {"STATUS", std::string(status)},
        {"MESSAGE_HEX", detail::hex_encode(message)},
        {"ELAPSED_MS", std::to_string(receipt.elapsed_ms)},
        {"EXIT_CODE", std::to_string(receipt.exit_code)},
        {"HANDLE_HEX", detail::hex_encode(receipt.handle)},
        {"OUTPUT_PRESENT", receipt.output ? "1" : "0"}};
    if (receipt.output)
        append_manifest(fields, "OUTPUT_", *receipt.output);
    fields.emplace_back("METADATA_COUNT", std::to_string(receipt.metadata.size()));
    std::size_t metadata_index = 0;
    for (const auto& item : receipt.metadata) {
        const std::string prefix = "METADATA_" + std::to_string(metadata_index++) + "_";
        fields.emplace_back(prefix + "KEY_HEX", detail::hex_encode(item.first));
        fields.emplace_back(prefix + "VALUE_HEX", detail::hex_encode(item.second));
    }
    detail::write_record(state_root_ / "runs" / run_id_ /
                             (detail::hash_text(receipt.target) + ".txt"),
                         fields);
}

inline engine::built engine::build_node(const std::string& name) {
    const auto found = rules_.find(name);
    if (found == rules_.end()) {
        const auto source = resolve(name);
        detail::captured_artifact captured;
        if (current_.dry_run) {
            captured = detail::capture_artifact(source);
        } else {
            detail::workspace_lock cache_guard(state_root_ / "cache.lock", true, true);
            captured = detail::import_artifact(source, state_root_);
        }
        if (current_.dry_run || current_.why)
            emit({event_kind::current, name, {}, "source", {}, captured.manifest});
        return {manifest_identity(captured.manifest), captured.manifest, false, false};
    }

    const rule& selected = found->second;
    std::vector<built> dependencies;
    dependencies.reserve(selected.dependencies.size());
    bool dependency_would_change = false;
    for (const auto& dependency : selected.dependencies) {
        const auto result = results_.find(dependency);
        if (result == results_.end())
            throw error("Dependency '" + dependency + "' failed before '" + name + "'");
        dependencies.push_back(result->second);
        dependency_would_change = dependency_would_change || result->second.would_change;
    }
    if (selected.recipes.empty()) {
        detail::fingerprint identity;
        identity.add("0mk-alias-v2");
        identity.add(selected.target);
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            identity.add(selected.dependencies[i]);
            identity.add(dependencies[i].identity);
        }
        return {identity.finish(), std::nullopt, dependency_would_change, false};
    }
    return execute_rule(selected, dependencies);
}

inline engine::built engine::execute_rule(const rule& selected,
                                          const std::vector<built>& dependencies) {
    if (current_.dry_run) {
        std::string waiting;
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            if (!dependencies[i].would_change)
                continue;
            if (!waiting.empty())
                waiting.push_back(',');
            waiting += selected.dependencies[i];
        }
        if (!waiting.empty()) {
            detail::fingerprint provisional;
            provisional.add("0mk-provisional-v1");
            provisional.add(selected.target);
            for (const auto& dependency : dependencies)
                provisional.add(dependency.identity);
            emit({event_kind::deferred, selected.target, selected.profile,
                  "waiting for dependencies", {{"waiting", waiting}}, std::nullopt, {}});
            return {provisional.finish(), std::nullopt, true, !selected.action};
        }
    }
    const auto profile_it = profiles_.find(selected.profile);
    if (profile_it == profiles_.end())
        throw error("Unknown executor profile '@" + selected.profile + "' for " + selected.target);
    const executor& implementation = profile_it->second;
    if (static_cast<int>(current_.cache) > static_cast<int>(implementation.guarantee))
        throw error("Executor '@" + selected.profile +
                    "' does not provide the requested " + std::string(name(current_.cache)) +
                    " cache identity");

    const auto mapped = mapped_inputs(selected, dependencies);
    const auto commands = expand_commands(selected, mapped, selected.target,
                                          implementation.compatibility_shell);
    invocation call;
    call.target = selected.target;
    call.commands = commands;
    call.source_root = root_;
    call.environment = environment_fingerprint();
    call.cache = current_.cache;
    call.submit = current_.submit;
    std::size_t artifact_index = 0;
    for (std::size_t i = 0; i < dependencies.size(); ++i) {
        if (!dependencies[i].artifact)
            continue;
        call.inputs.push_back({selected.dependencies[i], mapped.at(artifact_index++),
                               *dependencies[i].artifact, !dependencies[i].provisional,
                               dependencies[i].provisional});
    }
    for (const auto& command : call.commands) {
        if (command.kind != command_kind::argv || command.argv.empty())
            continue;
        const std::filesystem::path program(command.argv.front());
        if (program.is_absolute() || !program.has_parent_path())
            continue;
        bool unsafe = false;
        for (const auto& component : program)
            if (component == "..")
                unsafe = true;
        if (unsafe)
            throw error("Relative executables may not escape the project: " + program.string());
        const auto normalized = program.lexically_normal();
        const auto source = root_ / normalized;
        const auto captured = detail::capture_artifact(source);
        if (captured.manifest.kind != artifact_kind::file)
            throw error("Command is not a file artifact: " + program.string());
        const std::string logical = "tool:" + normalized.generic_string();
        const bool already_present = std::any_of(call.inputs.begin(), call.inputs.end(),
                                                 [&](const artifact_input& input) {
                                                     return input.logical_name == logical;
                                                 });
        if (!already_present) {
            bool materialize = true;
            for (const auto& input : call.inputs) {
                if (normalized == input.local_path && input.artifact.kind == artifact_kind::file) {
                    if (input.artifact != captured.manifest)
                        throw error("Executable snapshot conflicts with artifact input: " +
                                    normalized.generic_string());
                    materialize = false;
                    break;
                }
                if (input.artifact.kind == artifact_kind::tree &&
                    detail::path_has_prefix(normalized, input.local_path)) {
                    const auto relative = normalized.lexically_relative(input.local_path);
                    std::vector<detail::tree_entry> entries;
                    try {
                        entries = detail::read_tree(state_root_, input.artifact.digest);
                    } catch (...) {
                        if (!current_.dry_run)
                            throw;
                        const auto live = detail::capture_artifact(root_ /
                            std::filesystem::path(input.logical_name));
                        if (live.manifest != input.artifact)
                            throw error("Artifact input changed while planning: " +
                                        input.logical_name);
                        entries = live.entries;
                    }
                    const auto member = std::find_if(entries.begin(), entries.end(),
                        [&](const detail::tree_entry& entry) {
                            return entry.path == relative.generic_string();
                        });
                    const artifact_manifest member_manifest{
                        artifact_kind::file,
                        member == entries.end() ? std::string{} : member->digest,
                        member == entries.end() ? 0U : member->size,
                        member == entries.end() ? 0U : member->mode};
                    if (member == entries.end() || member->kind != artifact_kind::file ||
                        member_manifest != captured.manifest)
                        throw error("Executable snapshot conflicts with artifact tree input: " +
                                    normalized.generic_string());
                    materialize = false;
                    break;
                }
                if (detail::path_has_prefix(input.local_path, normalized))
                    throw error("Executable path overlaps artifact input: " +
                                normalized.generic_string() + " and " +
                                input.local_path.generic_string());
            }
            call.inputs.push_back(
                {logical, normalized, captured.manifest, true, false, materialize});
        }
    }
    const std::string executor_identity = implementation.fingerprint
                                              ? implementation.fingerprint(call)
                                              : detail::hash_text(implementation.cache_identity);
    if (!detail::valid_digest(executor_identity))
        throw error("Executor '@" + selected.profile + "' returned an invalid fingerprint");
    call.task_key = task_identity(selected, implementation, call, dependencies,
                                  executor_identity);
    call.attempt = run_id_;
    call.workspace = state_root_ / "work" / call.task_key;
    call.working_directory = call.workspace / "root";
    if (!selected.action)
        call.output = call.working_directory / std::filesystem::path(selected.target);
    for (auto& input : call.inputs)
        input.local_path = call.working_directory / input.local_path;

    detail::workspace_lock cache_guard(state_root_ / "cache.lock", !current_.dry_run, true);
    detail::workspace_lock task_guard(state_root_ / "locks" / ("task-" + call.task_key),
                                      !current_.dry_run);
    detail::workspace_lock target_guard(
        state_root_ / "locks" / ("target-" + detail::hash_text(selected.target)),
        !current_.dry_run);

    const receipt_data exact = read_receipt(call.task_key);
    const auto previous = previous_receipt(selected.target);
    const bool reusable = !selected.action && current_.cache != cache_policy::off &&
                          !current_.force && exact.valid && exact.output &&
                          exact.exit_code == 0 && detail::artifact_available(*exact.output, state_root_);
    if (reusable) {
        const bool published = detail::path_matches(resolve_output(selected.target), *exact.output);
        if (!published && !current_.dry_run)
            detail::materialize_artifact(*exact.output, state_root_,
                                         resolve_output(selected.target));
        event value{published ? event_kind::current : event_kind::restored,
                    selected.target,
                    selected.profile,
                    published ? (current_.why ? "task identity and output match receipt" : "")
                              : (current_.dry_run ? "would restore from cache"
                                                  : "restored from cache"),
                    {},
                    *exact.output};
        emit(std::move(value));
        if (!current_.dry_run) {
            write_head(selected.target, call.task_key);
            write_attempt(exact, published ? "current" : "restored");
        }
        return {manifest_identity(*exact.output), *exact.output, false, false};
    }

    mk0::plan preview;
    if (implementation.plan)
        preview = implementation.plan(call);
    if (selected.action)
        preview.warnings.emplace_back(
            "actions run in a private workspace; relative filesystem side effects are discarded");
    if (preview.estimated_cost_usd)
        preview.facts["cost_usd"] = [&] {
            std::ostringstream formatted;
            formatted << std::fixed << std::setprecision(2) << *preview.estimated_cost_usd;
            return formatted.str();
        }();
    for (const auto& warning : preview.warnings)
        emit({event_kind::warning, selected.target, selected.profile, warning, {}, std::nullopt,
              {}});
    auto reasons = explain_changes(previous ? &*previous : nullptr, selected, call,
                                   dependencies, executor_identity);
    if (!previous && !detail::read_record(head_path(selected.target)).empty())
        reasons = {"invalid receipt"};
    std::string reason;
    for (const auto& item : reasons) {
        if (!reason.empty())
            reason += "; ";
        reason += item;
    }
    if (current_.why)
        preview.facts["why"] = reason;
    if (current_.dry_run) {
        emit({event_kind::run, selected.target, selected.profile, {}, preview.facts,
              std::nullopt, {}});
        return {call.task_key, std::nullopt, true, !selected.action};
    }
    if (preview.requires_submit && !current_.submit) {
        emit({event_kind::plan, selected.target, selected.profile,
              "remote execution requires --submit", preview.facts, std::nullopt, {}});
        throw approval_required("Target '" + selected.target + "' requires --submit");
    }

    std::error_code cleanup_error;
    detail::remove_tree(call.workspace, cleanup_error);
    if (cleanup_error)
        throw error("Cannot clean task workspace: " + cleanup_error.message());
    std::filesystem::create_directories(call.working_directory, cleanup_error);
    if (cleanup_error)
        throw error("Cannot create task workspace: " + cleanup_error.message());
    try {
        for (const auto& input : call.inputs) {
            if (detail::starts_with(input.logical_name, "tool:")) {
                const auto imported = detail::import_artifact(
                    root_ / std::filesystem::path(input.logical_name.substr(5)), state_root_);
                if (imported.manifest != input.artifact)
                    throw error("Executable changed while it was snapshotted: " +
                                input.logical_name.substr(5));
            }
            if (input.materialize)
                detail::materialize_artifact(input.artifact, state_root_, input.local_path, true);
        }
        if (!selected.action)
            detail::ensure_parent(call.output);
        emit({event_kind::run, selected.target, selected.profile, {}, preview.facts, std::nullopt,
              {}});
        const auto started = std::chrono::steady_clock::now();
        result outcome = implementation.run(call, [&](std::string_view line) {
            emit({event_kind::log, selected.target, selected.profile, std::string(line), {},
                  std::nullopt, {}});
        });
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);

        receipt_data receipt;
        receipt.task_key = call.task_key;
        receipt.target = selected.target;
        receipt.profile = selected.profile;
        receipt.executor_identity = executor_identity;
        for (const auto& command : call.commands) {
            if (!receipt.canonical_command.empty())
                receipt.canonical_command.push_back('\n');
            if (command.kind == command_kind::shell) {
                receipt.canonical_command += command.shell;
            } else {
                for (const auto& argument : command.argv) {
                    if (!receipt.canonical_command.empty() &&
                        receipt.canonical_command.back() != '\n')
                        receipt.canonical_command.push_back(' ');
                    receipt.canonical_command += detail::shell_quote(argument);
                }
            }
        }
        receipt.command_digest = command_digest(call.commands);
        detail::fingerprint environment;
        for (const auto& item : call.environment) {
            environment.add(item.first);
            environment.add(item.second);
        }
        receipt.environment_digest = environment.finish();
        receipt.cache = current_.cache;
        receipt.elapsed_ms = static_cast<std::uint64_t>(elapsed.count());
        receipt.exit_code = outcome.exit_code;
        receipt.metadata = outcome.metadata;
        for (const auto& fact : preview.facts)
            if (fact.first != "why")
                receipt.metadata.emplace("plan." + fact.first, fact.second);
        receipt.handle = outcome.handle;
        for (std::size_t i = 0; i < dependencies.size(); ++i)
            receipt.inputs.push_back(
                {selected.dependencies[i], dependencies[i].identity, dependencies[i].artifact});
        for (const auto& input : call.inputs)
            if (detail::starts_with(input.logical_name, "tool:"))
                receipt.inputs.push_back(
                    {input.logical_name, manifest_identity(input.artifact), input.artifact});
        if (!outcome.success()) {
            write_attempt(receipt, "failed", outcome.metadata.count("error")
                                                    ? outcome.metadata.at("error")
                                                    : "executor failed");
            throw error("Target '" + selected.target + "' failed with exit code " +
                        std::to_string(outcome.exit_code) +
                        (outcome.metadata.count("error") ? ": " + outcome.metadata.at("error")
                                                         : ""));
        }

        if (implementation.fingerprint && implementation.fingerprint(call) != executor_identity)
            throw error("Execution environment changed while building '" + selected.target + "'");
        if (environment_fingerprint() != call.environment)
            throw error("Declared environment changed while building '" + selected.target + "'");

        if (selected.action) {
            receipt.valid = true;
            write_receipt(receipt);
            write_head(selected.target, call.task_key);
            write_attempt(receipt, "done");
            detail::remove_tree(call.workspace, cleanup_error);
            if (cleanup_error)
                throw error("Cannot clean task workspace: " + cleanup_error.message());
            emit({event_kind::done, selected.target, selected.profile, {}, {}, std::nullopt,
                  elapsed});
            return {call.task_key, std::nullopt, true, false};
        }

        const auto output_status = std::filesystem::symlink_status(call.output);
        if (std::filesystem::is_symlink(output_status) ||
            (!std::filesystem::is_regular_file(output_status) &&
             !std::filesystem::is_directory(output_status)))
            throw error("Target '" + selected.target +
                        "' succeeded but did not create a file or tree artifact at $@");
        const auto captured = detail::import_artifact(call.output, state_root_);
        receipt.output = captured.manifest;
        if (current_.cache != cache_policy::off && exact.valid && exact.output &&
            *exact.output != captured.manifest) {
            receipt.metadata["previous_output"] = exact.output->digest;
            receipt.metadata["new_output"] = captured.manifest.digest;
            write_attempt(receipt, "nondeterministic", "same task key produced a different output");
            throw error("Nondeterministic target '" + selected.target +
                        "': the same task key produced different output");
        }
        detail::materialize_artifact(captured.manifest, state_root_,
                                     resolve_output(selected.target));
        receipt.valid = true;
        write_receipt(receipt);
        write_head(selected.target, call.task_key);
        write_attempt(receipt, "done");
        detail::remove_tree(call.workspace, cleanup_error);
        if (cleanup_error)
            throw error("Cannot clean task workspace: " + cleanup_error.message());
        emit({event_kind::done, selected.target, selected.profile, {}, {}, captured.manifest,
              elapsed});
        return {manifest_identity(captured.manifest), captured.manifest, true, false};
    } catch (...) {
        std::error_code ignored;
        detail::remove_tree(call.workspace, ignored);
        throw;
    }
}

inline int engine::evaluate() {
    bool failed = false;
    for (const auto& level : levels_) {
        if (detail::received_signal.load(std::memory_order_relaxed))
            break;
        const auto& names = level.second;
        for (std::size_t begin = 0; begin < names.size(); begin += current_.jobs) {
            if (detail::received_signal.load(std::memory_order_relaxed))
                break;
            const std::size_t end = std::min(names.size(), begin + current_.jobs);
            std::vector<std::future<std::pair<std::string, built>>> futures;
            futures.reserve(end - begin);
            for (std::size_t i = begin; i < end; ++i) {
                if (detail::received_signal.load(std::memory_order_relaxed))
                    break;
                const std::string node = names[i];
                if (current_.jobs == 1) {
                    std::promise<std::pair<std::string, built>> promise;
                    try {
                        promise.set_value({node, build_node(node)});
                    } catch (...) {
                        promise.set_exception(std::current_exception());
                    }
                    futures.push_back(promise.get_future());
                } else {
                    futures.push_back(std::async(std::launch::async, [this, node] {
                        return std::make_pair(node, build_node(node));
                    }));
                }
            }
            std::exception_ptr approval;
            std::vector<std::pair<std::string, built>> completed_nodes;
            completed_nodes.reserve(futures.size());
            for (auto& future : futures) {
                try {
                    completed_nodes.push_back(future.get());
                } catch (const approval_required&) {
                    approval = std::current_exception();
                    failed = true;
                } catch (const std::exception& failure) {
                    emit({event_kind::failure, {}, {}, failure.what(), {}, std::nullopt, {}});
                    failed = true;
                }
            }
            for (auto& completed : completed_nodes)
                results_[std::move(completed.first)] = std::move(completed.second);
            if (detail::received_signal.load(std::memory_order_relaxed))
                break;
            if (approval)
                std::rethrow_exception(approval);
            if (failed && !current_.keep_going)
                return static_cast<int>(exit_status::task_failed);
        }
    }
    return failed ? static_cast<int>(exit_status::task_failed)
                  : static_cast<int>(exit_status::success);
}

inline int engine::run(run_options selected) {
    detail::signal_scope signals;
    try {
        const int status = run_impl(std::move(selected));
        const int interrupted = detail::received_signal.load(std::memory_order_relaxed);
        return interrupted ? 128 + interrupted : status;
    } catch (const approval_required& failure) {
        const int interrupted = detail::received_signal.load(std::memory_order_relaxed);
        if (interrupted)
            return 128 + interrupted;
        emit({event_kind::failure, {}, {}, failure.what(), {}, std::nullopt, {}});
        return static_cast<int>(exit_status::approval_required);
    } catch (const adapter_failure& failure) {
        const int interrupted = detail::received_signal.load(std::memory_order_relaxed);
        if (interrupted)
            return 128 + interrupted;
        emit({event_kind::failure, {}, {}, failure.what(), {}, std::nullopt, {}});
        return static_cast<int>(exit_status::task_failed);
    } catch (...) {
        const int interrupted = detail::received_signal.load(std::memory_order_relaxed);
        if (interrupted)
            return 128 + interrupted;
        throw;
    }
}

inline int engine::run_impl(run_options selected) {
    current_ = std::move(selected);
    if (!current_.jobs)
        throw error("jobs must be positive");
    if (current_.jobs > 256)
        throw error("jobs may not exceed 256");
    configure_paths(current_);
    (void)prepare_state(false);
    if (current_.command == operation::cache_verify || current_.command == operation::cache_du ||
        current_.command == operation::cache_gc)
        return cache_command(current_.command);

    load(current_.file);
    if (current_.command == operation::targets)
        return list_targets();
    for (auto& target : current_.targets) {
        if (detail::ends_with(target, '!'))
            target.pop_back();
        target = detail::normalize_reference(std::move(target));
    }
    if (current_.command == operation::inspect)
        return inspect_target(current_.targets.front());
    if (current_.targets.empty())
        current_.targets.push_back(order_.front());
    ensure_profiles();
    validate_and_schedule(current_.targets);
    for (const auto& level : levels_)
        for (const auto& name_value : level.second) {
            const auto selected_rule = rules_.find(name_value);
            if (selected_rule != rules_.end() && !selected_rule->second.recipes.empty() &&
                static_cast<int>(current_.cache) >
                    static_cast<int>(profiles_.at(selected_rule->second.profile).guarantee))
                throw error("Executor '@" + selected_rule->second.profile +
                            "' does not provide the requested " +
                            std::string(name(current_.cache)) + " cache identity");
        }
    if (!current_.dry_run)
        (void)prepare_state(true);
    results_.clear();
    run_id_ = current_.dry_run ? std::string{} : detail::hash_text(detail::unique_suffix());
    return evaluate();
}

inline int engine::list_targets() {
    for (const auto& name_value : order_) {
        const auto& selected = rules_.at(name_value);
        std::string dependencies;
        for (const auto& dependency : selected.dependencies) {
            if (!dependencies.empty())
                dependencies.push_back(',');
            dependencies += dependency;
        }
        std::map<std::string, std::string> facts;
        facts.emplace("kind", selected.action ? "action"
                                               : (selected.recipes.empty() ? "alias" : "artifact"));
        if (!dependencies.empty())
            facts.emplace("dependencies", dependencies);
        emit({event_kind::target, selected.target, selected.recipes.empty() ? "" : selected.profile,
              {}, std::move(facts), std::nullopt, {}});
    }
    return static_cast<int>(exit_status::success);
}

inline int engine::inspect_target(const std::string& target) {
    const auto previous = previous_receipt(target);
    if (!previous)
        throw error("No receipt for target '" + target + "'");
    const auto& receipt = *previous;
    std::map<std::string, std::string> facts{
        {"task", "sha256:" + receipt.task_key},
        {"executor", receipt.executor_identity},
        {"command", receipt.canonical_command},
        {"cache", std::string(name(receipt.cache))},
        {"elapsed_ms", std::to_string(receipt.elapsed_ms)},
        {"exit", std::to_string(receipt.exit_code)}};
    if (receipt.output) {
        facts.emplace("output", "sha256:" + receipt.output->digest);
        facts.emplace("output_kind", std::string(name(receipt.output->kind)));
        facts.emplace("output_size", std::to_string(receipt.output->size));
        facts.emplace("output_mode", std::to_string(receipt.output->mode));
    }
    for (const auto& input : receipt.inputs)
        facts.emplace("input." + input.name,
                      input.artifact ? "sha256:" + input.artifact->digest : input.identity);
    for (const auto& metadata : receipt.metadata)
        facts.emplace("metadata." + metadata.first, metadata.second);
    if (!receipt.handle.empty()) {
        facts.emplace("handle", receipt.handle);
        const auto executable = detail::find_executable("0mk-exec-" + receipt.profile, root_);
        if (!executable.empty()) {
            try {
                const auto response = detail::request_process(
                    {executable.string(), "inspect"}, root_,
                    detail::protocol_request("inspect", receipt.profile, nullptr,
                                             receipt.handle));
                const auto inspected =
                    detail::protocol_response(response, "inspect", receipt.profile);
                const auto remote_facts =
                    detail::json_string_map(detail::json_find(inspected, "facts"));
                for (const auto& item : remote_facts)
                    facts["remote." + item.first] = item.second;
                if (const auto* state = detail::json_find(inspected, "state"))
                    facts["remote.state"] = state->as_string();
                if (const auto* exit_code = detail::json_find(inspected, "exit_code")) {
                    const double raw_exit = exit_code->as_number();
                    if (!std::isfinite(raw_exit) ||
                        raw_exit < static_cast<double>(std::numeric_limits<int>::min()) ||
                        raw_exit > static_cast<double>(std::numeric_limits<int>::max()) ||
                        raw_exit != static_cast<double>(static_cast<int>(raw_exit)))
                        throw error("invalid inspect exit_code");
                    facts["remote.exit_code"] = std::to_string(static_cast<int>(raw_exit));
                }
                if (const auto* metadata = detail::json_find(inspected, "metadata"))
                    for (const auto& item : metadata->as_object())
                        facts["remote.metadata." + item.first] = item.second.as_string();
            } catch (const adapter_failure&) {
                throw;
            } catch (const std::exception& failure) {
                throw adapter_failure("Executor '" + receipt.profile +
                                      "' returned invalid inspect data: " + failure.what());
            }
        }
    }
    emit({event_kind::inspect, target, receipt.profile, {}, std::move(facts), receipt.output,
          std::chrono::milliseconds(receipt.elapsed_ms)});
    return static_cast<int>(exit_status::success);
}

inline bool engine::verify_receipt_artifacts(const receipt_data& receipt) const {
    if (receipt.output && !detail::artifact_available(*receipt.output, state_root_))
        return false;
    for (const auto& input : receipt.inputs)
        if (input.artifact && !detail::artifact_available(*input.artifact, state_root_))
            return false;
    return true;
}

inline std::set<std::string> engine::referenced_blobs() const {
    std::set<std::string> referenced;
    const auto mark = [&](const artifact_manifest& artifact) {
        if (artifact.kind == artifact_kind::file) {
            referenced.insert("blob:" + artifact.digest);
        } else {
            referenced.insert("tree:" + artifact.digest);
            for (const auto& entry : detail::read_tree(state_root_, artifact.digest))
                if (entry.kind == artifact_kind::file)
                    referenced.insert("blob:" + entry.digest);
        }
    };
    const auto receipts = state_root_ / "receipts";
    if (std::filesystem::is_directory(receipts)) {
        for (const auto& entry : std::filesystem::directory_iterator(receipts)) {
            if (!entry.is_regular_file())
                throw error("Cannot garbage collect with an invalid receipt entry: " +
                            entry.path().string());
            const receipt_data receipt = read_receipt(entry.path().stem().string());
            if (!receipt.valid)
                throw error("Cannot garbage collect with an invalid receipt: " +
                            entry.path().string());
            if (receipt.output)
                mark(*receipt.output);
            for (const auto& input : receipt.inputs)
                if (input.artifact)
                    mark(*input.artifact);
        }
    }
    const auto runs = state_root_ / "runs";
    if (std::filesystem::is_directory(runs)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(runs)) {
            if (!entry.is_regular_file())
                continue;
            const auto record = detail::read_record(entry.path());
            const std::string output_present = record_value(record, "OUTPUT_PRESENT");
            if (record_value(record, "VERSION") != "1" ||
                (output_present != "0" && output_present != "1"))
                throw error("Cannot garbage collect with an invalid run record: " +
                            entry.path().string());
            if (output_present == "0")
                continue;
            mark(parse_manifest(record, "OUTPUT_"));
        }
    }
    return referenced;
}

inline int engine::cache_command(operation selected) {
    if (!prepare_state(false)) {
        if (selected == operation::cache_verify)
            emit({event_kind::cache, "verify", {}, "verified",
                  {{"checked", "0"}, {"errors", "0"}}, std::nullopt, {}});
        else if (selected == operation::cache_du)
            emit({event_kind::cache, "du", {}, {}, {{"bytes", "0"}, {"files", "0"}},
                  std::nullopt, {}});
        else
            emit({event_kind::cache, "gc", {}, {}, {{"removed", "0"}, {"bytes", "0"}},
                  std::nullopt, {}});
        return static_cast<int>(exit_status::success);
    }
    detail::workspace_lock lock(state_root_ / "cache.lock", true,
                                selected == operation::cache_du);
    if (selected == operation::cache_du) {
        std::uint64_t bytes = 0;
        std::uint64_t files = 0;
        if (std::filesystem::is_directory(state_root_)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(state_root_)) {
                if (entry.is_regular_file()) {
                    std::error_code size_error;
                    const auto size = entry.file_size(size_error);
                    if (!size_error)
                        bytes += size;
                    ++files;
                }
            }
        }
        emit({event_kind::cache, "du", {}, {},
              {{"bytes", std::to_string(bytes)}, {"files", std::to_string(files)}},
              std::nullopt, {}});
        return static_cast<int>(exit_status::success);
    }

    if (selected == operation::cache_verify) {
        std::uint64_t checked = 0;
        std::uint64_t errors = 0;
        const auto receipts = state_root_ / "receipts";
        if (std::filesystem::is_directory(receipts)) {
            for (const auto& entry : std::filesystem::directory_iterator(receipts)) {
                if (!entry.is_regular_file()) {
                    ++errors;
                    continue;
                }
                const receipt_data receipt = read_receipt(entry.path().stem().string());
                ++checked;
                if (!receipt.valid || !verify_receipt_artifacts(receipt))
                    ++errors;
            }
        }
        const auto targets = state_root_ / "targets";
        if (std::filesystem::is_directory(targets)) {
            for (const auto& entry : std::filesystem::directory_iterator(targets)) {
                ++checked;
                try {
                    if (!entry.is_regular_file()) {
                        ++errors;
                        continue;
                    }
                    const auto head = detail::read_record(entry.path());
                    const std::string target =
                        detail::hex_decode(record_value(head, "TARGET_HEX"));
                    const receipt_data receipt = read_receipt(record_value(head, "TASK_KEY"));
                    if (record_value(head, "VERSION") != "1" || target.empty() ||
                        entry.path().stem().string() != detail::hash_text(target) ||
                        !receipt.valid || receipt.target != target)
                        ++errors;
                } catch (...) {
                    ++errors;
                }
            }
        }
        const auto runs = state_root_ / "runs";
        if (std::filesystem::is_directory(runs)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(runs)) {
                if (!entry.is_regular_file())
                    continue;
                const auto record = detail::read_record(entry.path());
                ++checked;
                const std::string output_present = record_value(record, "OUTPUT_PRESENT");
                if (record_value(record, "VERSION") != "1" ||
                    (output_present != "0" && output_present != "1")) {
                    ++errors;
                    continue;
                }
                if (output_present == "0")
                    continue;
                try {
                    if (!detail::artifact_available(parse_manifest(record, "OUTPUT_"),
                                                    state_root_))
                        ++errors;
                } catch (...) {
                    ++errors;
                }
            }
        }
        const auto objects = state_root_ / "objects";
        if (std::filesystem::is_directory(objects)) {
            bool safe_fanout = true;
            for (const auto& entry : std::filesystem::directory_iterator(objects)) {
                ++checked;
                const std::string name_value = entry.path().filename().string();
                const bool valid_name = name_value.size() == 2 &&
                                        std::all_of(name_value.begin(), name_value.end(), [](char c) {
                                            return (c >= '0' && c <= '9') ||
                                                   (c >= 'a' && c <= 'f');
                                        });
                if (!valid_name || entry.is_symlink() || !entry.is_directory()) {
                    ++errors;
                    safe_fanout = false;
                }
            }
            if (safe_fanout) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(objects)) {
                    if (entry.is_directory() && !entry.is_symlink())
                        continue;
                    const std::string digest = entry.path().parent_path().filename().string() +
                                               entry.path().filename().string();
                    ++checked;
                    if (entry.is_symlink() || !entry.is_regular_file() ||
                        !detail::valid_digest(digest) ||
                        !detail::valid_blob(entry.path(), digest))
                        ++errors;
                }
            }
        }
        const auto trees = state_root_ / "trees";
        if (std::filesystem::is_directory(trees)) {
            for (const auto& entry : std::filesystem::directory_iterator(trees)) {
                const std::string digest = entry.path().stem().string();
                ++checked;
                try {
                    if (entry.is_symlink() || !entry.is_regular_file() ||
                        !detail::valid_digest(digest))
                        ++errors;
                    else
                        (void)detail::read_tree(state_root_, digest);
                } catch (...) {
                    ++errors;
                }
            }
        }
        emit({event_kind::cache, "verify", {}, errors ? "cache corruption detected" : "verified",
              {{"checked", std::to_string(checked)}, {"errors", std::to_string(errors)}},
              std::nullopt, {}});
        return errors ? static_cast<int>(exit_status::cache_corrupt)
                      : static_cast<int>(exit_status::success);
    }

    const auto referenced = referenced_blobs();
    std::uint64_t removed = 0;
    std::uint64_t bytes = 0;
    const auto remove_unreferenced = [&](const std::filesystem::path& directory,
                                         std::string_view prefix, bool recursive) {
        if (!std::filesystem::is_directory(directory))
            return;
        std::vector<std::filesystem::path> candidates;
        if (recursive) {
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                const std::string name_value = entry.path().filename().string();
                const bool valid_name = name_value.size() == 2 &&
                                        std::all_of(name_value.begin(), name_value.end(), [](char c) {
                                            return (c >= '0' && c <= '9') ||
                                                   (c >= 'a' && c <= 'f');
                                        });
                if (!valid_name || entry.is_symlink() || !entry.is_directory())
                    throw error("Unsafe cache object fanout: " + entry.path().string());
            }
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                if (entry.is_directory() && !entry.is_symlink())
                    continue;
                if (entry.is_symlink() || !entry.is_regular_file())
                    throw error("Unsafe cache object entry: " + entry.path().string());
                candidates.push_back(entry.path());
            }
        } else {
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                if (entry.is_symlink() || !entry.is_regular_file())
                    throw error("Unsafe cache manifest entry: " + entry.path().string());
                candidates.push_back(entry.path());
            }
        }
        for (const auto& path : candidates) {
            const std::string digest = recursive
                                           ? path.parent_path().filename().string() +
                                                 path.filename().string()
                                           : path.stem().string();
            if (referenced.count(std::string(prefix) + digest))
                continue;
            std::error_code size_error;
            const auto size = std::filesystem::file_size(path, size_error);
            std::error_code remove_error;
            if (std::filesystem::remove(path, remove_error)) {
                ++removed;
                if (!size_error)
                    bytes += size;
            } else if (remove_error) {
                throw error("Cannot remove cache object " + path.string() + ": " +
                            remove_error.message());
            }
        }
    };
    remove_unreferenced(state_root_ / "objects", "blob:", true);
    remove_unreferenced(state_root_ / "trees", "tree:", false);
    std::error_code cleanup_error;
    detail::remove_tree(state_root_ / "work", cleanup_error);
    if (cleanup_error)
        throw error("Cannot clean cache workspaces: " + cleanup_error.message());
    emit({event_kind::cache, "gc", {}, {},
          {{"removed", std::to_string(removed)}, {"bytes", std::to_string(bytes)}},
          std::nullopt, {}});
    return static_cast<int>(exit_status::success);
}

} // namespace mk0

#endif // NJLANE314_0MK_H_INCLUDED
