#pragma once
#include <algorithm>
#include <cwctype>
#include <set>
#include <string>
#include <vector>

namespace edit {
using Text = std::wstring;
struct Range {
    int a, b;
};
struct Change {
    Range old;
    Text text;
    Range selection;
};
inline int length(const Text &s) { return static_cast<int>(s.size()); }
inline bool word(wchar_t c) { return iswalnum(c) || c == L'_' || c >= 128; }
inline int start(const Text &s, int p) {
    while (p > 0 && s[p - 1] != L'\n')
        --p;
    return p;
}
inline int end(const Text &s, int p) {
    while (p < length(s) && s[p] != L'\n')
        ++p;
    return p;
}
inline Range lines(const Text &s, Range r) {
    int last = r.b > r.a && s[r.b - 1] == L'\n' ? r.b - 1 : r.b;
    return {start(s, r.a), end(s, last)};
}
inline Range wordAt(const Text &s, int p) {
    int a = p, b = p;
    while (a && word(s[a - 1]))
        --a;
    while (b < length(s) && word(s[b]))
        ++b;
    return {a, b};
}
inline Change insert(Range r, Text value, int offset = 0) {
    int p = r.a + length(value) + offset;
    return {r, std::move(value), {p, p}};
}
enum class Block { indent, outdent, comment };
inline Change block(const Text &s, Range r, Block op) {
    Range span = lines(s, r);
    Text out;
    bool uncomment = true, hasText = false;
    for (int i = span.a; i <= span.b;) {
        int e = end(s, i), j = i;
        while (j < e && (s[j] == ' ' || s[j] == '\t'))
            ++j;
        if (j < e) {
            hasText = true;
            if (s[j] != '#')
                uncomment = false;
        }
        i = e + 1;
    }
    for (int i = span.a; i <= span.b;) {
        int e = end(s, i);
        Text line = s.substr(i, e - i);
        size_t first = line.find_first_not_of(L" \t");
        if (op == Block::indent)
            line.insert(0, L"    ");
        if (op == Block::outdent) {
            size_t n = 0;
            if (!line.empty() && line[0] == '\t')
                n = 1;
            else
                while (n < 4 && n < line.size() && line[n] == ' ')
                    ++n;
            line.erase(0, n);
        }
        if (op == Block::comment && (first != Text::npos || !hasText)) {
            if (first == Text::npos)
                first = line.size();
            if (uncomment && hasText)
                line.erase(first, first + 1 < line.size() && line[first + 1] == ' ' ? 2 : 1);
            else
                line.insert(first, L"# ");
        }
        out += line;
        if (e < span.b)
            out += L'\n';
        i = e + 1;
    }
    int delta = length(out) - (span.b - span.a);
    Range sel = r.a == r.b ? Range{std::clamp(r.a + delta, span.a, span.a + length(out)), 0}
                           : Range{span.a, span.a + length(out) + (r.b > span.b ? 1 : 0)};
    if (r.a == r.b)
        sel.b = sel.a;
    return {span, out, sel};
}
inline Change moveLines(const Text &s, Range r, bool down, bool duplicate) {
    Range span = lines(s, r);
    Text current = s.substr(span.a, span.b - span.a);
    if (duplicate) {
        int delta = down ? length(current) + 1 : 0;
        return {span, current + L"\n" + current, {r.a + delta, r.b + delta}};
    }
    if (down && span.b < length(s)) {
        int e = end(s, span.b + 1);
        Text next = s.substr(span.b + 1, e - span.b - 1);
        int d = length(next) + 1;
        return {{span.a, e}, next + L"\n" + current, {r.a + d, std::min(length(s), r.b + d)}};
    }
    if (!down && span.a > 0) {
        int p = start(s, span.a - 1), d = span.a - p;
        return {{p, span.b}, current + L"\n" + s.substr(p, d - 1), {r.a - d, r.b - d}};
    }
    return {{r.a, r.a}, L"", r};
}
enum class Ink { normal, keyword, builtin, number, string, comment, definition };
struct Token {
    int a, b;
    Ink ink;
};
inline const std::set<Text> keywords = {
    L"False",   L"None",     L"True",     L"and",    L"as",   L"assert", L"async",  L"await",
    L"break",   L"class",    L"continue", L"def",    L"del",  L"elif",   L"else",   L"except",
    L"finally", L"for",      L"from",     L"global", L"if",   L"import", L"in",     L"is",
    L"lambda",  L"nonlocal", L"not",      L"or",     L"pass", L"raise",  L"return", L"try",
    L"while",   L"with",     L"yield",    L"match",  L"case"};
inline const std::set<Text> builtins = {
    L"abs",    L"all",       L"any",        L"bin",   L"bool",      L"bytes",     L"chr",     L"dict",
    L"divmod", L"enumerate", L"filter",     L"float", L"format",    L"frozenset", L"getattr", L"hex",
    L"input",  L"int",       L"isinstance", L"iter",  L"len",       L"list",      L"map",     L"max",
    L"min",    L"next",      L"object",     L"open",  L"ord",       L"pow",       L"print",   L"range",
    L"repr",   L"reversed",  L"round",      L"set",   L"slice",     L"sorted",    L"str",     L"sum",
    L"super",  L"tuple",     L"type",       L"zip",   L"Exception", L"ValueError"};
inline std::vector<Token> scan(const Text &s) {
    std::vector<Token> tokens;
    bool definition = false;
    for (int i = 0, n = length(s); i < n;) {
        int a = i;
        wchar_t c = s[i++];
        Ink ink = Ink::normal;
        if (c == '#') {
            i = end(s, i);
            ink = Ink::comment;
        } else if (c == '\'' || c == '"') {
            bool triple = i + 1 < n && s[i] == c && s[i + 1] == c;
            if (triple)
                i += 2;
            while (i < n) {
                if (s[i] == '\\') {
                    i = std::min(n, i + 2);
                    continue;
                }
                if (s[i] == c && (!triple || (i + 2 < n && s[i + 1] == c && s[i + 2] == c))) {
                    i += triple ? 3 : 1;
                    break;
                }
                if (!triple && s[i] == '\n')
                    break;
                ++i;
            }
            ink = Ink::string;
        } else if (iswdigit(c)) {
            while (i < n && (iswalnum(s[i]) || s[i] == '_' || s[i] == '.'))
                ++i;
            ink = Ink::number;
        } else if (word(c)) {
            while (i < n && word(s[i]))
                ++i;
            Text name = s.substr(a, i - a);
            if (definition)
                ink = Ink::definition;
            else if (keywords.count(name))
                ink = Ink::keyword;
            else if (builtins.count(name))
                ink = Ink::builtin;
            definition = name == L"def" || name == L"class";
        } else if (c == '\n')
            definition = false;
        if (ink != Ink::normal)
            tokens.push_back({a, i, ink});
    }
    return tokens;
}

// Ranges arrive in document order. Scan the prefix once so simultaneous cursors
// use the same pre-edit text, including the start of any continued statement.
inline std::vector<Text> lineBreaks(const Text &s, const std::vector<Range> &ranges) {
    if (ranges.empty())
        return {};
    Text syntax = s.substr(0, ranges.back().a);
    for (const auto &token : scan(syntax)) {
        if (token.ink == Ink::comment || token.ink == Ink::string) {
            // A literal is an operand even when it contains a colon, #, or a newline.
            std::fill(syntax.begin() + token.a, syntax.begin() + token.b,
                      token.ink == Ink::comment ? L' ' : L'~');
        }
    }

    std::vector<Text> breaks;
    breaks.reserve(ranges.size());
    int pos = 0, statement = 0, last = -1, depth = 0;
    bool hasSuiteColon = false;
    for (auto r : ranges) {
        while (pos < r.a) {
            wchar_t c = syntax[pos];
            if (c == L'\n') {
                if (!depth && (pos == 0 || syntax[pos - 1] != L'\\')) {
                    statement = pos + 1;
                    hasSuiteColon = false;
                }
                last = -1;
            } else if (!iswspace(c)) {
                if (wcschr(L"([{", c))
                    ++depth;
                else if (wcschr(L")]}", c))
                    depth = std::max(0, depth - 1);
                else if (!depth && c == L':' && (pos + 1 == length(syntax) || syntax[pos + 1] != L'='))
                    hasSuiteColon = true;
                last = pos;
            }
            ++pos;
        }

        int head = start(s, r.a);
        bool opensBlock = last >= head && !depth && syntax[last] == L':';
        if (last >= head && statement == head && !depth && !hasSuiteColon && syntax[last] != L'\\') {
            size_t tail = s.find_first_not_of(L" \t", r.b);
            if (tail == Text::npos || s[tail] == L'\n' || s[tail] == L'#') {
                auto first = syntax.find_first_not_of(L" \t", head);
                int wordEnd = static_cast<int>(first);
                while (wordEnd < r.a && word(syntax[wordEnd]))
                    ++wordEnd;
                Text name = syntax.substr(first, wordEnd - first);
                opensBlock = name == L"if" || name == L"elif" || name == L"for" || name == L"while";
            }
        }
        int base = opensBlock ? statement : head;
        int p = base;
        while (p < r.a && (s[p] == L' ' || s[p] == L'\t'))
            ++p;
        Text pad = s.substr(base, p - base);
        if (opensBlock || (last >= head && wcschr(L":([{", syntax[last])))
            pad += L"    ";
        breaks.push_back(L"\n" + pad);
    }
    return breaks;
}
inline Change newline(const Text &s, Range r) { return insert(r, lineBreaks(s, {r}).front()); }
} // namespace edit
