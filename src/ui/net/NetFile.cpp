// .net reader. A hand-written lexer and a statement-level parser: we never
// evaluate DX expressions, we only need to know which tokens make up a
// statement, which side of `=` they are on and which are call arguments.
//
// Semantics follow src/uipp/dxuilib/Network.C:
// - A `// node` comment starts a node; following comments belong to it until
//   the next node, decorator or the end-of-body marker.
// - Call arguments are numbered per node (1-based, across all of the node's
//   statements). An argument naming another node's output variable is a link.
// - A plain `x = y;` under a node links y to input 1 (Output, Transmitter).
// - A reference from a node to its own output is not a link (Colormap writes
//   `Colormap_1_out_1 = g(Colormap_1_out_1)`).
// Output variables are matched by full name. Interactors (Scalar, Selector, ...)
// have no statement in the body -- their outputs are assigned after the macro --
// so a name no statement wrote is matched against the pattern the UI writes,
// `<macro>_<Module>_<inst>_out_<k>` (or the pre-2.0 `<Module>_<inst>_out_<k>`).

#include "NetFile.h"

#include <algorithm>
#include <charconv>
#include <exception>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace dxui {

namespace {

// Nested `{ }` blocks inside a macro recurse; garbage input must not be able to
// exhaust the stack.
constexpr int kMaxBlockDepth = 64;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// ASCII-only classification: <cctype> is locale dependent and undefined for
// negative chars.
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isIdentStart(char c) { return isAlpha(c) || c == '_' || c == '@'; }
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c) || c == '\''; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view ltrim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    return s;
}
std::string_view rtrim(std::string_view s) {
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return s;
}
std::string_view trim(std::string_view s) { return rtrim(ltrim(s)); }
std::string_view stripCR(std::string_view s) {
    while (!s.empty() && s.back() == '\r') s.remove_suffix(1);
    return s;
}
bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

template <typename T>
bool toNumber(std::string_view s, T& v) {
    s = trim(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    T tmp{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), tmp);
    if (ec != std::errc() || p != s.data() + s.size()) return false;
    v = tmp;
    return true;
}

// `key = value, key = value, ..., last = rest of line`. The legacy reader uses
// fixed sscanf formats; this accepts the same lines and tolerates missing or
// reordered keys.
std::vector<std::pair<std::string_view, std::string_view>>
keyValues(std::string_view s, std::string_view restKey) {
    std::vector<std::pair<std::string_view, std::string_view>> kv;
    for (;;) {
        s = ltrim(s);
        std::size_t eq = s.find('=');
        if (s.empty() || eq == std::string_view::npos) break;
        std::string_view key = trim(s.substr(0, eq));
        s = ltrim(s.substr(eq + 1));
        if (key == restKey) {
            kv.emplace_back(key, rtrim(s));
            break;
        }
        std::size_t comma = s.find(',');
        kv.emplace_back(key, trim(s.substr(0, comma)));
        if (comma == std::string_view::npos) break;
        s = s.substr(comma + 1);
    }
    return kv;
}

// k for a name ending in `_out_<k>`, else 0. prefixLen gets the length up to
// and including `_out_`.
int outIndexOf(std::string_view name, std::size_t* prefixLen = nullptr) {
    std::size_t pos = name.rfind("_out_");
    if (pos == std::string_view::npos) return 0;
    std::string_view digits = name.substr(pos + 5);
    if (digits.empty()) return 0;
    for (char c : digits)
        if (!isDigit(c)) return 0;
    int k = 0;
    if (!toNumber(digits, k) || k <= 0) return 0;
    if (prefixLen) *prefixLen = pos + 5;
    return k;
}

// ---------------------------------------------------------------------------
// Lexer. Token rules from src/uipp/dxuilib/net.lex: `//`, `#` and `$` start a
// line comment; strings end on the same line and may contain \"; identifiers
// may contain @ and '.
// ---------------------------------------------------------------------------

enum class Tok { Ident, Number, String, Comment, Punct, End };

struct Token {
    Tok kind = Tok::End;
    std::string_view text;           // for a comment, the text after the marker
    std::size_t begin = 0, end = 0;  // source offsets of the whole token
    int line = 0;

    bool is(std::string_view p) const { return kind == Tok::Punct && text == p; }
    bool isIdent(std::string_view w) const { return kind == Tok::Ident && text == w; }
};

class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}

    // False on a lexical error; message() and line() say what and where.
    bool next(Token& t) {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == '\n') {
                ++line_;
                ++pos_;
            } else if (isSpace(c)) {
                ++pos_;
            } else {
                break;
            }
        }
        t = Token{};
        t.line = line_;
        t.begin = pos_;
        if (pos_ >= s_.size()) {
            t.end = pos_;
            return true;
        }
        char c = s_[pos_];
        char c1 = pos_ + 1 < s_.size() ? s_[pos_ + 1] : '\0';

        if ((c == '/' && c1 == '/') || c == '#' || c == '$') {
            std::size_t start = pos_ + (c == '/' ? 2 : 1);
            std::size_t e = s_.find('\n', pos_);
            if (e == std::string_view::npos) e = s_.size();
            t.kind = Tok::Comment;
            // `$sync` and friends are executive directives; the legacy lexer
            // hands them to the parser as empty comments.
            t.text = c == '$' ? std::string_view() : s_.substr(start, e - start);
            pos_ = e;
        } else if (c == '"') {
            std::size_t p = pos_ + 1;
            for (;;) {
                if (p >= s_.size() || s_[p] == '\n') return fail("unterminated string");
                if (s_[p] == '\\' && p + 1 < s_.size() && s_[p + 1] != '\n') {
                    p += 2;
                } else if (s_[p] == '"') {
                    ++p;
                    break;
                } else {
                    ++p;
                }
            }
            t.kind = Tok::String;
            pos_ = p;
        } else if (isIdentStart(c)) {
            std::size_t p = pos_ + 1;
            while (p < s_.size() && isIdentChar(s_[p])) ++p;
            t.kind = Tok::Ident;
            pos_ = p;
        } else if (isDigit(c) || (c == '.' && isDigit(c1))) {
            // Loose on purpose: values are kept as source text, never converted.
            std::size_t p = pos_ + 1;
            while (p < s_.size()) {
                char d = s_[p];
                bool exponentSign = (d == '+' || d == '-') && (s_[p - 1] == 'e' || s_[p - 1] == 'E');
                if (!(isDigit(d) || isAlpha(d) || d == '.' || d == '_' || exponentSign)) break;
                ++p;
            }
            t.kind = Tok::Number;
            pos_ = p;
        } else {
            static constexpr std::string_view two[] = {"->", "==", "!=", "<=", ">=", ":=", "..",
                                                       "++", "--", "**", "&&", "||", "<>"};
            std::size_t len = 0;
            for (std::string_view op : two)
                if (s_.substr(pos_, 2) == op) len = 2;
            if (len == 0) {
                bool printablePunct = c > ' ' && c < 0x7f && !isIdentChar(c) && c != '"';
                if (!printablePunct) {
                    static const char hex[] = "0123456789abcdef";
                    unsigned char u = static_cast<unsigned char>(c);
                    std::string msg = "unexpected character 0x";
                    msg += hex[u >> 4];
                    msg += hex[u & 15];
                    return fail(std::move(msg));
                }
                len = 1;
            }
            t.kind = Tok::Punct;
            pos_ += len;
        }
        t.end = pos_;
        if (t.kind != Tok::Comment) t.text = s_.substr(t.begin, t.end - t.begin);
        return true;
    }

    const std::string& message() const { return message_; }
    int line() const { return line_; }

private:
    bool fail(std::string msg) {
        message_ = std::move(msg);
        return false;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    int line_ = 1;
    std::string message_;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

struct OutRef {
    std::size_t node;
    int output;
};

struct InRef {
    std::size_t macro;
    std::size_t node;
    int input;
};

// An identifier used as an input, resolved once the whole macro is read.
struct PendingRef {
    std::string name;
    std::size_t node;
    int input;
    bool expression;  // from `x = y;` rather than a call argument
    int line;
};

struct MacroState {
    NetMacro m;
    std::size_t current = kNone;    // node receiving comments and statements
    std::size_t decorator = kNone;  // decorator receiving annotation lines
    bool endSeen = false;           // `// network: end of macro body`
    int argIndex = 0;               // running argument number of the current node
    std::vector<int> maxArg, maxOut;
    std::unordered_map<std::string, OutRef> outVars;  // lvalue name -> producer
    std::vector<PendingRef> refs;
};

class Parser {
public:
    Parser(std::string_view text, NetFile& out) : text_(text), lex_(text), out_(out) {}

    bool run();
    const NetParseError& error() const { return error_; }

private:
    // Token stream with one token of lookahead. A lexical error turns the
    // stream into End; lexFailed_ tells the two apart.
    void advance() {
        if (lexFailed_) return;
        if (!lex_.next(la_)) {
            lexFailed_ = true;
            int line = lex_.line();
            la_ = Token{};
            la_.line = line;
        }
    }
    const Token& peek() const { return la_; }
    Token take() {
        Token t = la_;
        advance();
        return t;
    }

    bool fail(int line, std::string message) {
        if (error_.message.empty()) {
            error_.line = line;
            error_.message = std::move(message);
        }
        return false;
    }
    // At End inside something that needs closing: report the lexical error if
    // that is what ended the stream.
    bool failEnd(std::string message) {
        if (lexFailed_) return fail(la_.line, lex_.message());
        return fail(la_.line, std::move(message));
    }
    void warn(int line, std::string_view message) {
        out_.warnings.push_back("line " + std::to_string(line) + ": " + std::string(message));
    }

    bool collectStatement(std::vector<Token>& st, int& errLine, std::string& errMsg);
    bool parseFormals(std::vector<NetFormal>& formals);
    bool parseMacro(int line, const std::vector<std::pair<int, std::string>>& header);
    bool parseBlock(MacroState& s, int depth);
    bool bodyComment(MacroState& s, const Token& t);
    bool startNode(MacroState& s, std::string_view rest, int line);
    void ioComment(MacroState& s, std::string_view c, int line, bool input);
    void statement(MacroState& s, const std::vector<Token>& st);
    void callArgs(MacroState& s, const std::vector<Token>& st, std::size_t first);
    void finishMacro(MacroState& s);
    void applyHeader(NetMacro& m, const std::vector<std::pair<int, std::string>>& header);
    void parseVersion(std::string_view s);
    void assignment(const std::vector<Token>& st);
    bool includeStatement();

    std::string_view text_;
    Lexer lex_;
    NetFile& out_;
    Token la_;
    bool lexFailed_ = false;
    bool lastMacroWasNetwork_ = false;
    NetParseError error_;
    std::unordered_map<std::string, InRef> inVars_;  // input variable -> node input
};

// Tokens up to the `;` that ends the statement at bracket depth 0.
bool Parser::collectStatement(std::vector<Token>& st, int& errLine, std::string& errMsg) {
    std::vector<char> open;
    for (;;) {
        const Token& t = peek();
        errLine = t.line;
        if (t.kind == Tok::End) {
            errMsg = lexFailed_ ? lex_.message() : "unexpected end of file, expected ';'";
            return false;
        }
        if (t.kind == Tok::Comment) {
            errMsg = "comment inside a statement (missing ';'?)";
            return false;
        }
        if (t.kind == Tok::Punct) {
            char c = t.text[0];
            if (t.text.size() == 1 && (c == '(' || c == '[' || c == '{')) {
                open.push_back(c == '(' ? ')' : c == '[' ? ']' : '}');
            } else if (t.text.size() == 1 && (c == ')' || c == ']' || c == '}')) {
                if (open.empty()) {
                    errMsg = c == '}' ? "expected ';' before '}'" : std::string("unbalanced '") + c + "'";
                    return false;
                }
                if (open.back() != c) {
                    errMsg = std::string("mismatched '") + c + "'";
                    return false;
                }
                open.pop_back();
            } else if (c == ';' && open.empty()) {
                take();
                return true;
            }
        }
        st.push_back(take());
    }
}

// `name`, `name = default` ... up to the closing parenthesis.
bool Parser::parseFormals(std::vector<NetFormal>& formals) {
    std::vector<Token> group;
    int depth = 0;
    auto flush = [&]() -> bool {
        if (group.empty()) return true;
        if (group[0].kind != Tok::Ident) return fail(group[0].line, "expected a macro parameter name");
        NetFormal f;
        f.name = std::string(group[0].text);
        std::size_t last = group.size();
        // A trailing `[visible: 0]` is an attribute, not part of the default.
        if (group.back().is("]")) {
            int d = 0;
            for (std::size_t i = group.size(); i-- > 1;) {
                if (group[i].is("]")) ++d;
                if (group[i].is("[") && --d == 0) {
                    bool attr = false;
                    for (std::size_t j = i; j < group.size(); ++j) attr = attr || group[j].is(":");
                    if (attr) last = i;
                    break;
                }
            }
        }
        if (last > 2 && group[1].is("=")) {
            std::size_t b = group[2].begin, e = group[last - 1].end;
            f.defaultValue = std::string(text_.substr(b, e - b));
        }
        formals.push_back(std::move(f));
        group.clear();
        return true;
    };
    for (;;) {
        const Token& t = peek();
        if (t.kind == Tok::End) return failEnd("unexpected end of file in macro parameters");
        if (t.kind == Tok::Comment) {
            take();
            continue;
        }
        if (t.is("(") || t.is("[") || t.is("{")) ++depth;
        if (t.is(")") || t.is("]") || t.is("}")) {
            if (depth == 0) {
                if (!t.is(")")) return fail(t.line, "unbalanced bracket in macro parameters");
                take();
                return flush();
            }
            --depth;
        }
        if (depth == 0 && t.is(",")) {
            take();
            if (!flush()) return false;
            continue;
        }
        group.push_back(take());
    }
}

void Parser::parseVersion(std::string_view s) {
    // "3.1.1 (format), 3.1.4 (DX)" -- either half may be missing.
    auto leadingVersion = [](std::string_view v) {
        v = ltrim(v);
        std::size_t n = 0;
        while (n < v.size() && (isDigit(v[n]) || v[n] == '.')) ++n;
        return std::string(v.substr(0, n));
    };
    out_.formatVersion = leadingVersion(s);
    std::size_t comma = s.find(',');
    if (comma != std::string_view::npos) out_.dxVersion = leadingVersion(s.substr(comma + 1));
}

void Parser::applyHeader(NetMacro& m, const std::vector<std::pair<int, std::string>>& header) {
    for (const auto& [line, text] : header) {
        std::string_view c = text;
        if (startsWith(c, "MODULE ")) {
            continue;  // the macro statement names it too
        } else if (startsWith(c, "CATEGORY ")) {
            m.category = std::string(trim(c.substr(9)));
        } else if (startsWith(c, "DESCRIPTION ")) {
            m.description = std::string(trim(c.substr(12)));
        } else if (startsWith(c, "comment:")) {
            c.remove_prefix(8);
            if (!c.empty() && c.front() == ' ') c.remove_prefix(1);
            m.comments.emplace_back(c);
        } else if (startsWith(c, "workspace:")) {
            for (const auto& [k, v] : keyValues(c.substr(10), {})) {
                if (k == "width" && !toNumber(v, m.workspaceWidth)) warn(line, "bad workspace width");
                if (k == "height" && !toNumber(v, m.workspaceHeight)) warn(line, "bad workspace height");
            }
        } else {
            m.headerComments.emplace_back(c);
        }
    }
}

// Top level: header comments, `include`, macros and the statements the
// executive needs (input assignments, `main();`). Every macro is parsed the same
// way; one with no node comments and no end-of-body marker was generated for
// the executive (the `macro Image(...)` wrappers) and is dropped. Once a
// network has been read, damage further on only ends the read with a warning:
// the legacy reader never looks past the first macro.
bool Parser::run() {
    advance();
    std::vector<std::pair<int, std::string>> header;  // comments awaiting their macro
    bool haveNetwork = false;
    auto tolerate = [&]() {
        if (!haveNetwork) return false;
        warn(error_.line, "after the network: " + error_.message + " (rest of file ignored)");
        error_ = NetParseError{};
        return true;
    };
    for (;;) {
        if (lexFailed_) {
            fail(la_.line, lex_.message());
            if (!tolerate()) return false;
            break;
        }
        const Token& t = peek();
        if (t.kind == Tok::End) break;
        if (t.kind == Tok::Comment) {
            std::string_view c = stripCR(ltrim(t.text));
            if (startsWith(c, "version:")) {
                if (out_.formatVersion.empty()) parseVersion(c.substr(8));
            } else if (!trim(c).empty() && !startsWith(c, "time:")) {
                header.emplace_back(t.line, std::string(c));
            }
            take();
            continue;
        }
        if (t.isIdent("include")) {
            if (includeStatement()) continue;
            if (!tolerate()) return false;
            break;
        }
        if (t.isIdent("macro")) {
            int line = take().line;
            bool ok = parseMacro(line, header);
            header.clear();
            if (ok) {
                haveNetwork = haveNetwork || lastMacroWasNetwork_;
                continue;
            }
            if (!tolerate()) return false;
            break;
        }
        std::vector<Token> st;
        int errLine = 0;
        std::string errMsg;
        if (!collectStatement(st, errLine, errMsg)) {
            fail(errLine, errMsg);
            if (!tolerate()) return false;
            break;
        }
        assignment(st);
    }
    if (out_.macros.empty()) return fail(la_.line, "no macro definition found");
    return true;
}

bool Parser::parseMacro(int line, const std::vector<std::pair<int, std::string>>& header) {
    MacroState s;
    s.m.line = line;
    Token name = take();
    if (name.kind != Tok::Ident) return failEnd("expected a macro name after 'macro'");
    s.m.name = std::string(name.text);
    if (!peek().is("(")) return failEnd("expected '(' after macro " + s.m.name);
    take();
    if (!parseFormals(s.m.formalInputs)) return false;
    if (peek().is("->")) {
        take();
        if (!peek().is("(")) return failEnd("expected '(' after '->' in macro " + s.m.name);
        take();
        if (!parseFormals(s.m.formalOutputs)) return false;
    }
    if (peek().is("[")) {  // macro attributes
        while (!peek().is("]")) {
            if (peek().kind == Tok::End) return failEnd("unexpected end of file in macro attributes");
            take();
        }
        take();
    }
    if (!peek().is("{")) return failEnd("expected '{' to start macro " + s.m.name);
    take();
    applyHeader(s.m, header);
    if (!parseBlock(s, 1)) return false;
    finishMacro(s);
    lastMacroWasNetwork_ = s.endSeen || !s.m.nodes.empty();
    if (lastMacroWasNetwork_) out_.macros.push_back(std::move(s.m));
    return true;
}

// Statements and comments up to and including the closing '}'.
bool Parser::parseBlock(MacroState& s, int depth) {
    if (depth > kMaxBlockDepth) return fail(peek().line, "blocks nested too deeply");
    for (;;) {
        Token t = peek();
        if (t.kind == Tok::End) return failEnd("unexpected end of file inside macro " + s.m.name);
        if (t.is("}")) {
            take();
            return true;
        }
        if (t.is("{")) {
            take();
            if (!parseBlock(s, depth + 1)) return false;
            continue;
        }
        if (t.kind == Tok::Comment) {
            take();
            if (!bodyComment(s, t)) return false;
            continue;
        }
        if (t.isIdent("macro")) return fail(t.line, "macro definition inside macro " + s.m.name);
        if (t.isIdent("include")) {
            if (!includeStatement()) return false;
            continue;
        }
        std::vector<Token> st;
        int errLine = 0;
        std::string errMsg;
        if (!collectStatement(st, errLine, errMsg)) return fail(errLine, errMsg);
        statement(s, st);
    }
}

bool Parser::bodyComment(MacroState& s, const Token& t) {
    std::string_view c = stripCR(ltrim(t.text));
    if (trim(c).empty()) return true;
    if (startsWith(c, "node ")) return startNode(s, c.substr(5), t.line);
    if (rtrim(c) == "network: end of macro body") {
        s.endSeen = true;
        s.current = s.decorator = kNone;
        return true;
    }
    if (startsWith(c, "decorator")) {
        s.m.decorators.push_back(NetDecorator{t.line, {std::string(c)}});
        s.decorator = s.m.decorators.size() - 1;
        s.current = kNone;
        return true;
    }
    // Legacy routes these, and any "group:" line right after a decorator, to
    // the decorator.
    bool decoratorLine = startsWith(c, "annotation") || startsWith(c, "resource");
    if (s.decorator != kNone && (decoratorLine || c.find("group:") != std::string_view::npos)) {
        s.m.decorators[s.decorator].comments.emplace_back(c);
        return true;
    }
    s.decorator = kNone;
    if (startsWith(c, "comment:")) {
        c.remove_prefix(8);
        if (!c.empty() && c.front() == ' ') c.remove_prefix(1);
        s.m.comments.emplace_back(c);
        return true;
    }
    if (s.current == kNone) {
        s.m.headerComments.emplace_back(c);
        return true;
    }
    if (startsWith(c, "input[")) {
        ioComment(s, c, t.line, true);
    } else if (startsWith(c, "output[")) {
        ioComment(s, c, t.line, false);
    } else {
        NetNode& n = s.m.nodes[s.current];
        n.extraComments.emplace_back(c);
        if (startsWith(c, "page group:")) n.pageGroup = std::string(trim(c.substr(11)));
    }
    return true;
}

// "Module[inst]: x = X, y = Y, inputs = N[, outputs = M], label = L"
bool Parser::startNode(MacroState& s, std::string_view rest, int line) {
    s.current = s.decorator = kNone;
    std::size_t lb = rest.find('[');
    std::size_t rb = rest.find(']');
    if (lb == std::string_view::npos || rb == std::string_view::npos || rb < lb)
        return fail(line, "cannot parse node comment");
    NetNode n;
    n.line = line;
    n.module = std::string(trim(rest.substr(0, lb)));
    if (n.module.empty() || !toNumber(rest.substr(lb + 1, rb - lb - 1), n.instance))
        return fail(line, "cannot parse node comment");
    rest = ltrim(rest.substr(rb + 1));
    if (rest.empty() || rest.front() != ':') return fail(line, "cannot parse node comment");
    bool haveLabel = false;
    for (const auto& [k, v] : keyValues(rest.substr(1), "label")) {
        bool ok = true;
        if (k == "x") {
            ok = toNumber(v, n.x);
        } else if (k == "y") {
            ok = toNumber(v, n.y);
        } else if (k == "inputs") {
            ok = n.inputsFromComment = toNumber(v, n.numInputs);
        } else if (k == "outputs") {
            ok = n.outputsFromComment = toNumber(v, n.numOutputs);
        } else if (k == "label") {
            n.label = std::string(v);
            haveLabel = true;
        }
        if (!ok) warn(line, "bad '" + std::string(k) + "' in node comment");
    }
    if (!haveLabel) n.label = n.module;
    s.m.nodes.push_back(std::move(n));
    s.maxArg.push_back(0);
    s.maxOut.push_back(0);
    s.current = s.m.nodes.size() - 1;
    s.argIndex = 0;
    return true;
}

// "input[i]: defaulting = d, visible = v, type = t, value = ..." or older
// subsets such as "input[i]: visible = v". Damage here is not fatal (the legacy
// reader carries on too); the line is kept verbatim in extraComments.
void Parser::ioComment(MacroState& s, std::string_view c, int line, bool input) {
    NetNode& n = s.m.nodes[s.current];
    std::size_t lb = c.find('['), rb = c.find(']');
    int index = 0;
    std::string_view rest;
    if (rb != std::string_view::npos && rb > lb) rest = ltrim(c.substr(rb + 1));
    if (rest.empty() || rest.front() != ':' || !toNumber(c.substr(lb + 1, rb - lb - 1), index) || index <= 0) {
        warn(line, input ? "cannot parse input comment" : "cannot parse output comment");
        n.extraComments.emplace_back(c);
        return;
    }
    int limit = input ? (n.inputsFromComment ? n.numInputs : -1) : (n.outputsFromComment ? n.numOutputs : -1);
    if (limit >= 0 && index > limit)
        warn(line, std::string(input ? "input" : "output") + " index " + std::to_string(index) + " beyond the node's count");

    bool visible = true, defaulting = true, hasValue = false;
    int type = 0;
    std::string value;
    for (const auto& [k, v] : keyValues(rest.substr(1), "value")) {
        int iv = 0;
        bool ok = true;
        if (k == "defaulting") {
            ok = toNumber(v, iv);
            defaulting = iv != 0;
        } else if (k == "visible") {
            ok = toNumber(v, iv);
            visible = iv != 0;
        } else if (k == "type") {
            ok = toNumber(v, type);
        } else if (k == "value") {
            value = std::string(v);
            hasValue = true;
        }
        if (!ok) warn(line, "bad '" + std::string(k) + "' in i/o comment");
    }
    if (input) {
        n.inputs.push_back(NetInput{index, visible, defaulting, type, std::move(value), hasValue});
    } else {
        n.outputs.push_back(NetOutput{index, visible, type, std::move(value), hasValue});
        s.maxOut[s.current] = std::max(s.maxOut[s.current], index);
    }
}

void Parser::statement(MacroState& s, const std::vector<Token>& st) {
    // Statements before the first node, and the CacheScene() calls after the
    // end-of-body marker, belong to no node.
    if (s.current == kNone || st.empty()) return;

    std::size_t eq = kNone;
    int depth = 0;
    for (std::size_t i = 0; i < st.size(); ++i) {
        const Token& t = st[i];
        if (t.is("(") || t.is("[") || t.is("{")) ++depth;
        if (t.is(")") || t.is("]") || t.is("}")) --depth;
        if (depth == 0 && (t.is("=") || t.is(":="))) {
            eq = i;
            break;
        }
    }

    std::size_t rhs = 0;
    if (eq != kNone) {
        // Output variables, each possibly followed by `[cache: n]`.
        depth = 0;
        for (std::size_t i = 0; i < eq; ++i) {
            const Token& t = st[i];
            if (t.is("[")) ++depth;
            if (t.is("]")) --depth;
            if (depth != 0 || t.kind != Tok::Ident) continue;
            if (int k = outIndexOf(t.text)) {
                s.outVars.emplace(std::string(t.text), OutRef{s.current, k});
                s.maxOut[s.current] = std::max(s.maxOut[s.current], k);
            }
        }
        rhs = eq + 1;
    }

    if (rhs + 1 < st.size() && st[rhs].kind == Tok::Ident && st[rhs + 1].is("(")) {
        callArgs(s, st, rhs);
    } else if (eq != kNone) {
        for (std::size_t i = rhs; i < st.size(); ++i)
            if (st[i].kind == Tok::Ident)
                s.refs.push_back(PendingRef{std::string(st[i].text), s.current, 1, true, st[i].line});
    }
}

// `Module(arg, ..., arg)` starting at st[first]; trailing attributes ignored.
void Parser::callArgs(MacroState& s, const std::vector<Token>& st, std::size_t first) {
    std::size_t i = first + 2;
    if (i < st.size() && st[i].is(")")) return;  // no arguments
    std::vector<const Token*> arg;
    int depth = 0;
    auto flush = [&]() {
        int index = ++s.argIndex;
        s.maxArg[s.current] = std::max(s.maxArg[s.current], index);
        std::size_t b = 0;
        if (arg.size() >= 2 && arg[0]->kind == Tok::Ident && arg[1]->is("=")) b = 2;  // name = value
        if (arg.size() == b + 1 && arg[b]->kind == Tok::Ident)
            s.refs.push_back(PendingRef{std::string(arg[b]->text), s.current, index, false, arg[b]->line});
        arg.clear();
    };
    for (; i < st.size(); ++i) {
        const Token& t = st[i];
        if (t.is("(") || t.is("[") || t.is("{")) ++depth;
        if (t.is(")") || t.is("]") || t.is("}")) {
            if (depth == 0) {
                flush();
                return;
            }
            --depth;
        }
        if (depth == 0 && t.is(",")) {
            flush();
            continue;
        }
        arg.push_back(&t);
    }
}

void Parser::finishMacro(MacroState& s) {
    NetMacro& m = s.m;
    if (m.nodes.empty()) return;
    std::unordered_map<std::string, std::size_t> byPrefix;
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        std::string tail = m.nodes[i].module + "_" + std::to_string(m.nodes[i].instance) + "_out_";
        if (!byPrefix.emplace(m.name + "_" + tail, i).second)
            warn(m.nodes[i].line, "duplicate node " + m.nodes[i].module + "[" +
                                      std::to_string(m.nodes[i].instance) + "]");
        byPrefix.emplace(tail, i);  // pre-2.0 names carry no macro prefix
    }
    for (const PendingRef& r : s.refs) {
        std::size_t from = kNone;
        int k = 0;
        if (auto it = s.outVars.find(r.name); it != s.outVars.end()) {
            from = it->second.node;
            k = it->second.output;
        } else {
            std::size_t len = 0;
            k = outIndexOf(r.name, &len);
            if (k > 0) {
                auto p = byPrefix.find(r.name.substr(0, len));
                if (p != byPrefix.end()) from = p->second;
            }
        }
        if (from != kNone) {
            if (from == r.node) continue;  // a node reading back its own output
            m.links.push_back(NetLink{from, k, r.node, r.input});
            s.maxOut[from] = std::max(s.maxOut[from], k);
            const NetNode& to = m.nodes[r.node];
            if (to.inputsFromComment && r.input > to.numInputs)
                warn(r.line, "link to input " + std::to_string(r.input) + " of " + to.module + "[" +
                                 std::to_string(to.instance) + "], which has " + std::to_string(to.numInputs));
        } else if (k > 0) {
            warn(r.line, "no node produces '" + r.name + "'");
        } else if (!r.expression && r.name.find("_in_") != std::string::npos) {
            inVars_.emplace(r.name, InRef{out_.macros.size(), r.node, r.input});
        }
    }
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        NetNode& n = m.nodes[i];
        int maxIn = s.maxArg[i];
        for (const NetInput& in : n.inputs) maxIn = std::max(maxIn, in.index);
        if (!n.inputsFromComment) n.numInputs = maxIn;
        if (!n.outputsFromComment) n.numOutputs = s.maxOut[i];
    }
}

// `include "file"` -- a statement with no ';' in the grammar. The UI writes one
// per referenced macro, after the `// macro reference` comment.
bool Parser::includeStatement() {
    take();
    Token name = take();
    if (name.kind != Tok::String) return failEnd("expected a file name after 'include'");
    std::string_view quoted = name.text.substr(1, name.text.size() - 2);
    out_.includes.emplace_back(quoted);
    return true;
}

// `input_variable = value;` at top level gives an unconnected input its value
// (the same value its `// input[i]:` comment usually shows).
void Parser::assignment(const std::vector<Token>& st) {
    if (st.size() < 3 || st[0].kind != Tok::Ident || !st[1].is("=")) return;
    auto it = inVars_.find(std::string(st[0].text));
    if (it == inVars_.end()) return;
    std::size_t b = st[2].begin, e = st.back().end;
    out_.macros[it->second.macro].nodes[it->second.node].assignedValues[it->second.input] =
        std::string(text_.substr(b, e - b));
}

}  // namespace

std::vector<NetWireless> NetMacro::wirelessPairs() const {
    std::vector<NetWireless> pairs;
    for (std::size_t r = 0; r < nodes.size(); ++r) {
        if (nodes[r].module != "Receiver") continue;
        for (std::size_t t = 0; t < nodes.size(); ++t)
            if (nodes[t].module == "Transmitter" && nodes[t].label == nodes[r].label)
                pairs.push_back(NetWireless{t, r});
    }
    return pairs;
}

const NetNode* NetMacro::findNode(std::string_view module, int instance) const {
    for (const NetNode& n : nodes)
        if (n.module == module && n.instance == instance) return &n;
    return nullptr;
}

const NetMacro* NetFile::findMacro(std::string_view name) const {
    for (const NetMacro& m : macros)
        if (m.name == name) return &m;
    return nullptr;
}

bool parseNet(std::string_view text, NetFile& out, NetParseError* err) {
    out = NetFile{};
    try {
        NetFile file;
        Parser p(text, file);
        if (!p.run()) {
            if (err) *err = p.error();
            return false;
        }
        out = std::move(file);
        return true;
    } catch (const std::exception& e) {  // bad_alloc, in practice
        if (err) *err = NetParseError{0, std::string("internal error: ") + e.what()};
    } catch (...) {
        if (err) *err = NetParseError{0, "internal error"};
    }
    return false;
}

bool parseNetFile(const std::filesystem::path& path, NetFile& out, NetParseError* err) {
    std::string text;
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            out = NetFile{};
            if (err) *err = NetParseError{0, "cannot open " + path.string()};
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad()) {
            out = NetFile{};
            if (err) *err = NetParseError{0, "cannot read " + path.string()};
            return false;
        }
    } catch (...) {
        out = NetFile{};
        if (err) *err = NetParseError{0, "cannot read " + path.string()};
        return false;
    }
    return parseNet(text, out, err);
}

}  // namespace dxui
