#include "script/Edit.hpp"

#include "support/Source.hpp"
#include "syntax/Lexer.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <vector>

namespace qiln::edit {

namespace {

using quartz::syntax::Tok;
using quartz::syntax::Token;

// The tokens of a script; they point into the source manager and the arena, so all three live together.
struct Lexed {
    quartz::SourceManager sources;
    quartz::Arena arena;
    std::vector<Token> tokens;
};

// One element of a `dependencies([...])` list, or the argument of `p.dependency(...)`, as token indices.
struct Element {
    std::size_t first = 0;
    std::size_t last = 0;
    std::size_t statement = 0; // first token of the statement that holds it
    std::size_t close = 0;     // the `)` that ends that statement
    std::size_t list = 0;      // the `[` of its list; 0 for `p.dependency(...)`
    std::size_t siblings = 0;  // elements in its list
};

struct Script {
    std::unique_ptr<Lexed> lexed;
    std::string_view text;
    std::size_t bodyOpen = 0;
    std::size_t bodyClose = 0;
    std::string_view param;
    std::vector<std::pair<std::size_t, std::size_t>> lists; // `[` and `]` of each `p.dependencies([...])`
    std::vector<Element> elements;
    std::vector<std::size_t> singles; // the `)` closing each `p.dependency(...)`

    [[nodiscard]] const Token& at(std::size_t i) const { return lexed->tokens[i]; }
};

bool opens(Tok t) { return t == Tok::LParen || t == Tok::LBracket || t == Tok::LBrace; }

bool closes(Tok t) { return t == Tok::RParen || t == Tok::RBracket || t == Tok::RBrace; }

// The index of the token that closes the one at `open`, or nullopt.
std::optional<std::size_t> matching(const std::vector<Token>& toks, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < toks.size(); ++i) {
        depth += opens(toks[i].kind) ? 1 : closes(toks[i].kind) ? -1 : 0;
        if (depth == 0) {
            return i;
        }
    }
    return std::nullopt;
}

std::size_t lineStart(std::string_view text, std::size_t pos) {
    std::size_t nl = pos == 0 ? std::string_view::npos : text.rfind('\n', pos - 1);
    return nl == std::string_view::npos ? 0 : nl + 1;
}

std::size_t lineEnd(std::string_view text, std::size_t pos) {
    std::size_t nl = text.find('\n', pos);
    return nl == std::string_view::npos ? text.size() : nl;
}

std::string_view indentOf(std::string_view text, std::size_t pos) {
    std::size_t start = lineStart(text, pos);
    std::size_t end = start;
    while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) {
        ++end;
    }
    return text.substr(start, end - start);
}

bool blankBefore(std::string_view text, std::size_t pos) {
    std::size_t start = lineStart(text, pos);
    return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(start),
                       text.begin() + static_cast<std::ptrdiff_t>(pos), [](char c) { return c == ' ' || c == '\t'; });
}

// Only spaces or a line comment from `pos` to the end of its line.
bool blankAfter(std::string_view text, std::size_t pos) {
    std::string_view rest = text.substr(pos, lineEnd(text, pos) - pos);
    std::size_t i = rest.find_first_not_of(" \t\r");
    return i == std::string_view::npos || rest.substr(i).starts_with("//");
}

std::expected<Script, std::string> parse(std::string_view text) {
    Script s;
    s.text = text;
    s.lexed = std::make_unique<Lexed>();
    const quartz::SourceFile& file = s.lexed->sources.add("build.qz", std::string(text));
    quartz::Diagnostics diags(s.lexed->sources);
    s.lexed->tokens = quartz::syntax::tokenize(file, s.lexed->arena, diags);
    const auto& t = s.lexed->tokens;
    if (diags.hasErrors() || t.empty()) {
        return std::unexpected("build.qz has syntax errors; fix them before editing it");
    }
    std::optional<std::size_t> open;
    for (std::size_t i = 0; i + 4 < t.size() && !open; ++i) {
        if (t[i].kind == Tok::KwFn && t[i + 1].kind == Tok::Ident && t[i + 1].text == "build" &&
            t[i + 2].kind == Tok::LParen && t[i + 3].kind == Tok::Ident && t[i + 4].kind == Tok::Colon) {
            s.param = t[i + 3].text;
            auto params = matching(t, i + 2);
            for (std::size_t j = params ? *params : t.size(); j < t.size() && !open; ++j) {
                if (t[j].kind == Tok::LBrace) {
                    open = j;
                }
            }
        }
    }
    auto close = open ? matching(t, *open) : std::nullopt;
    if (!close) {
        return std::unexpected("build.qz has no fn build(p: Project) { ... } to edit");
    }
    s.bodyOpen = *open;
    s.bodyClose = *close;
    for (std::size_t i = s.bodyOpen + 1; i + 3 < s.bodyClose; ++i) {
        bool call = t[i].kind == Tok::Ident && t[i].text == s.param && t[i + 1].kind == Tok::Dot &&
                    t[i + 2].kind == Tok::Ident && t[i + 3].kind == Tok::LParen;
        if (!call || (t[i + 2].text != "dependencies" && t[i + 2].text != "dependency")) {
            continue;
        }
        auto end = matching(t, i + 3);
        if (!end || *end >= s.bodyClose) {
            continue;
        }
        if (t[i + 2].text == "dependency") {
            if (*end > i + 4) {
                s.elements.push_back({i + 4, *end - 1, i, *end, 0, 1});
                s.singles.push_back(*end);
            }
            continue;
        }
        auto listEnd = t[i + 4].kind == Tok::LBracket ? matching(t, i + 4) : std::nullopt;
        if (!listEnd || *listEnd + 1 != *end) {
            continue;
        }
        s.lists.emplace_back(i + 4, *listEnd);
        std::size_t before = s.elements.size();
        std::size_t first = i + 5;
        for (std::size_t j = i + 5; j <= *listEnd; ++j) {
            if (opens(t[j].kind) && j != *listEnd) {
                j = matching(t, j).value_or(*listEnd);
                continue;
            }
            if (t[j].kind == Tok::Comma || j == *listEnd) {
                if (j > first) {
                    s.elements.push_back({first, j - 1, i, *end, i + 4, 0});
                }
                first = j + 1;
            }
        }
        for (std::size_t k = before; k < s.elements.size(); ++k) {
            s.elements[k].siblings = s.elements.size() - before;
        }
    }
    return s;
}

// The package or project path a notation names: `org.example.text` of `org.example.text:^1.2`.
std::string_view subject(std::string_view notation) {
    return notation.starts_with(':') ? notation : notation.substr(0, notation.find(':'));
}

// The element that names `target`, by the string literal inside it.
std::optional<std::size_t> find(const Script& s, std::string_view target) {
    for (std::size_t k = 0; k < s.elements.size(); ++k) {
        for (std::size_t i = s.elements[k].first; i <= s.elements[k].last; ++i) {
            if (s.at(i).kind == Tok::String && subject(s.at(i).text) == target) {
                return k;
            }
        }
    }
    return std::nullopt;
}

std::string render(const Dependency& d) {
    if (d.notation.starts_with(':')) {
        return d.test ? std::format("Dependency(\"{}\", configuration = \"test\")", d.notation)
                      : std::format("project(\"{}\")", d.notation);
    }
    return std::format("{}(\"{}\")", d.test ? "testImplementation" : "implementation", d.notation);
}

std::string splice(std::string_view text, std::size_t begin, std::size_t end, std::string_view with) {
    std::string out(text.substr(0, begin));
    out.append(with).append(text.substr(end));
    return out;
}

std::size_t lineOf(std::string_view text, std::size_t pos) {
    return static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(pos), '\n')) +
           1;
}

std::string insert(const Script& s, const std::string& element) {
    std::string_view text = s.text;
    const auto& t = s.lexed->tokens;
    if (!s.lists.empty()) {
        auto [open, close] = s.lists.front();
        const Token& last = t[close - 1];
        bool empty = close == open + 1;
        if (lineOf(text, last.span.end) == lineOf(text, t[close].span.begin)) {
            if (empty) {
                std::string inner = std::format("\n{0}    {1},\n{0}", indentOf(text, t[open].span.begin), element);
                return splice(text, t[open].span.end, t[close].span.begin, inner);
            }
            std::string add = last.kind == Tok::Comma ? " " + element : ", " + element;
            return splice(text, last.span.end, last.span.end, add);
        }
        std::size_t at = lineStart(text, t[close].span.begin);
        std::string indent = empty ? std::string(indentOf(text, t[close].span.begin)) + "    "
                                   : std::string(indentOf(text, t[s.elements.back().first].span.begin));
        for (const Element& e : s.elements) {
            if (e.list == open) {
                indent = std::string(indentOf(text, t[e.first].span.begin));
            }
        }
        std::string out = splice(text, at, at, std::format("{}{},\n", indent, element));
        if (!empty && last.kind != Tok::Comma) {
            out.insert(last.span.end, ",");
        }
        return out;
    }
    if (!s.singles.empty()) {
        const Element& e = *std::find_if(s.elements.begin(), s.elements.end(),
                                         [&](const Element& x) { return x.close == s.singles.back(); });
        std::size_t at = lineEnd(text, t[e.close].span.end);
        std::string line =
            std::format("\n{}{}.dependency({})", indentOf(text, t[e.statement].span.begin), s.param, element);
        return splice(text, at, at, line);
    }
    const Token& brace = t[s.bodyClose];
    std::string indent = s.bodyClose > s.bodyOpen + 1 ? std::string(indentOf(text, t[s.bodyClose - 1].span.begin))
                                                      : std::string(indentOf(text, brace.span.begin)) + "    ";
    if (blankBefore(text, brace.span.begin)) {
        std::string block = std::format("{0}{1}.dependencies([\n{0}    {2},\n{0}])\n", indent, s.param, element);
        std::size_t at = lineStart(text, brace.span.begin);
        return splice(text, at, at, block);
    }
    // A body on one line: the statements stay, the new call and the closing brace get lines of their own.
    std::string_view outer = indentOf(text, brace.span.begin);
    std::size_t at = brace.span.begin;
    while (at > 0 && text[at - 1] == ' ') {
        --at;
    }
    std::string block =
        std::format("\n{0}    {1}.dependencies([\n{0}        {2},\n{0}    ])\n{0}", outer, s.param, element);
    return splice(text, at, brace.span.begin, block);
}

// The bytes from `begin` to `end`, widened to whole lines when nothing else shares them.
std::pair<std::size_t, std::size_t> widen(std::string_view text, std::size_t begin, std::size_t end) {
    if (blankBefore(text, begin) && blankAfter(text, end)) {
        std::size_t stop = lineEnd(text, end);
        return {lineStart(text, begin), stop < text.size() ? stop + 1 : stop};
    }
    return {begin, end};
}

} // namespace

std::expected<Edited, std::string> addDependency(std::string_view script, const Dependency& d) {
    auto s = parse(script);
    if (!s) {
        return std::unexpected(s.error());
    }
    std::string element = render(d);
    if (auto k = find(*s, subject(d.notation))) {
        const Element& e = s->elements[*k];
        std::size_t begin = s->at(e.first).span.begin;
        std::size_t end = s->at(e.last).span.end;
        if (script.substr(begin, end - begin) == element) {
            return Edited{std::string(script), Change::Unchanged};
        }
        bool simple = e.last == e.first + 3 && s->at(e.first).kind == Tok::Ident &&
                      s->at(e.first + 1).kind == Tok::LParen && s->at(e.first + 2).kind == Tok::String;
        if (!simple) {
            return std::unexpected(std::format("build.qz:{} already names {} in a form qiln does not rewrite; edit it "
                                               "there",
                                               lineOf(script, begin), subject(d.notation)));
        }
        return Edited{splice(script, begin, end, element), Change::Updated};
    }
    return Edited{insert(*s, element), Change::Added};
}

std::expected<Edited, std::string> removeDependency(std::string_view script, std::string_view target) {
    auto s = parse(script);
    if (!s) {
        return std::unexpected(s.error());
    }
    auto k = find(*s, target);
    if (!k) {
        return std::unexpected(std::format("build.qz does not depend on {}", target));
    }
    const Element& e = s->elements[*k];
    const auto& t = s->lexed->tokens;
    if (e.siblings == 1) {
        auto [begin, end] = widen(script, t[e.statement].span.begin, t[e.close].span.end);
        return Edited{splice(script, begin, end, ""), Change::Removed};
    }
    bool commaAfter = t[e.last + 1].kind == Tok::Comma;
    std::size_t begin = t[e.first].span.begin;
    std::size_t end = commaAfter ? t[e.last + 1].span.end : t[e.last].span.end;
    if (blankBefore(script, begin) && blankAfter(script, end)) {
        auto [from, to] = widen(script, begin, end);
        return Edited{splice(script, from, to, ""), Change::Removed};
    }
    if (commaAfter) {
        end = std::min(script.find_first_not_of(' ', end), script.size());
    } else if (t[e.first - 1].kind == Tok::Comma) {
        begin = t[e.first - 1].span.begin;
    }
    return Edited{splice(script, begin, end, ""), Change::Removed};
}

} // namespace qiln::edit
