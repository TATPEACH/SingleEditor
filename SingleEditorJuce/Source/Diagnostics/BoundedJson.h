#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdlib>
#include <set>

namespace singleeditor::diagnostics
{
// Preflight before JUCE's DOM parser: bounded recursion/tokens/strings and no
// ambiguous duplicate keys (including differently escaped spellings).
class BoundedJson final
{
public:
    static constexpr size_t maximumBytes = 256 * 1024;
    static bool parse(const juce::MemoryBlock& bytes, juce::var& result)
    {
        result = juce::var();
        if (bytes.getSize() == 0 || bytes.getSize() > maximumBytes) return false;
        const auto* data = static_cast<const char*>(bytes.getData());
        if (!juce::CharPointer_UTF8::isValidString(data, static_cast<int>(bytes.getSize()))) return false;
        BoundedJson scanner(data, bytes.getSize());
        if (!scanner.value(0)) return false;
        scanner.space();
        if (scanner.pos != scanner.end) return false;
        return juce::JSON::parse(juce::String::fromUTF8(data, static_cast<int>(bytes.getSize())), result).wasOk();
    }
private:
    BoundedJson(const char* p, size_t size) : pos(p), end(p + size) {}
    const char* pos;
    const char* end;
    size_t tokens = 0;
    void space() { while (pos != end && (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n')) ++pos; }
    bool take(char c) { space(); if (pos == end || *pos != c) return false; ++pos; return true; }
    bool string(juce::String* decoded = nullptr)
    {
        space();
        if (pos == end || *pos != '"') return false;
        const auto* start = pos++;
        while (pos != end && pos - start <= 4096)
        {
            const auto c = static_cast<unsigned char>(*pos++);
            if (c == '"')
            {
                if (decoded != nullptr)
                {
                    const auto v = juce::JSON::fromString(juce::String::fromUTF8(start, static_cast<int>(pos - start)));
                    if (!v.isString()) return false;
                    *decoded = v.toString();
                }
                return true;
            }
            if (c < 0x20) return false;
            if (c == '\\')
            {
                if (pos == end) return false;
                const char escape = *pos++;
                if (escape == 'u')
                {
                    if (end - pos < 4) return false;
                    for (int i = 0; i < 4; ++i)
                        if (!((*pos >= '0' && *pos <= '9') || (*pos >= 'a' && *pos <= 'f') || (*pos >= 'A' && *pos <= 'F'))) return false;
                        else ++pos;
                }
                else if (escape != '"' && escape != '\\' && escape != '/' && escape != 'b'
                         && escape != 'f' && escape != 'n' && escape != 'r' && escape != 't') return false;
            }
        }
        return false;
    }
    bool value(int depth)
    {
        space();
        if (pos == end || depth > 16 || ++tokens > 16384) return false;
        if (*pos == '"') return string();
        if (*pos == '{')
        {
            ++pos;
            if (take('}')) return true;
            std::set<juce::String> keys;
            do
            {
                juce::String key;
                if (!string(&key) || !keys.insert(key).second || keys.size() > 256 || !take(':') || !value(depth + 1)) return false;
                if (take('}')) return true;
            } while (take(','));
            return false;
        }
        if (*pos == '[')
        {
            ++pos;
            if (take(']')) return true;
            do { if (!value(depth + 1)) return false; if (take(']')) return true; } while (take(','));
            return false;
        }
        for (const auto* literal : { "true", "false", "null" })
        {
            const auto size = std::strlen(literal);
            if (static_cast<size_t>(end - pos) >= size && std::memcmp(pos, literal, size) == 0)
            { pos += size; return true; }
        }
        const auto* start = pos;
        if (*pos == '-') ++pos;
        if (pos == end || *pos < '0' || *pos > '9') return false;
        if (*pos == '0') ++pos;
        else while (pos != end && *pos >= '0' && *pos <= '9') ++pos;
        if (pos != end && *pos == '.')
        {
            ++pos; const auto* first = pos;
            while (pos != end && *pos >= '0' && *pos <= '9') ++pos;
            if (pos == first) return false;
        }
        if (pos != end && (*pos == 'e' || *pos == 'E'))
        {
            ++pos;
            if (pos != end && (*pos == '+' || *pos == '-')) ++pos;
            const auto* first = pos;
            while (pos != end && *pos >= '0' && *pos <= '9') ++pos;
            if (pos == first) return false;
        }
        if (pos - start > 64) return false;
        const std::string number(start, pos);
        return std::isfinite(std::strtod(number.c_str(), nullptr));
    }
};
}
