#include "NetWriter.h"
#include "ModuleDefs.h"
#include "NetFile.h"
#include "NetImageMacro.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace dxui {

namespace {

void fail(std::string* error, std::string message)
{
    if (error)
        *error = std::move(message);
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isIdentStart(char c) { return isAlpha(c) || c == '_' || c == '@'; }
bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c) || c == '\''; }
bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view ltrim(std::string_view s)
{
    while (!s.empty() && isBlank(s.front()))
        s.remove_prefix(1);
    return s;
}
std::string_view rtrim(std::string_view s)
{
    while (!s.empty() && (isBlank(s.back()) || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}
std::string_view trim(std::string_view s) { return rtrim(ltrim(s)); }
bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool allBlank(std::string_view s)
{
    for (char c : s)
        if (!isBlank(c) && c != '\n')
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// Lexer: the token rules of NetFile.cpp (src/uipp/dxuilib/net.lex): `//`, `#`
// and `$` start a line comment; strings end on their line; identifiers may
// contain @ and '. Positions are all the writer needs.
// ---------------------------------------------------------------------------

enum class Tk { Ident, Number, String, Comment, Punct };

struct Tok {
    Tk kind = Tk::Punct;
    std::size_t begin = 0, end = 0;
    int line = 0;
    std::string_view text;  // the token; for a comment, the text after the marker

    bool is(std::string_view p) const { return kind == Tk::Punct && text == p; }
    bool isIdent(std::string_view w) const { return kind == Tk::Ident && text == w; }
};

// Tokens of s up to the first lexical error; *stop is that error's offset, or
// s.size().
std::vector<Tok> tokenize(std::string_view s, std::size_t* stop)
{
    std::vector<Tok> out;
    std::size_t pos = 0;
    int line = 1;
    for (;;) {
        while (pos < s.size() && (s[pos] == '\n' || isBlank(s[pos]))) {
            if (s[pos] == '\n')
                ++line;
            ++pos;
        }
        if (pos >= s.size())
            break;
        Tok t;
        t.begin = pos;
        t.line = line;
        char c = s[pos];
        char c1 = pos + 1 < s.size() ? s[pos + 1] : '\0';
        if ((c == '/' && c1 == '/') || c == '#' || c == '$') {
            std::size_t start = pos + (c == '/' ? 2 : 1);
            std::size_t e = s.find('\n', pos);
            if (e == std::string_view::npos)
                e = s.size();
            t.kind = Tk::Comment;
            t.text = c == '$' ? std::string_view() : s.substr(start, e - start);
            pos = e;
            t.end = pos;
            out.push_back(t);
            continue;
        }
        if (c == '"') {
            std::size_t p = pos + 1;
            for (;;) {
                if (p >= s.size() || s[p] == '\n') {
                    *stop = pos;
                    return out;
                }
                if (s[p] == '\\' && p + 1 < s.size() && s[p + 1] != '\n') {
                    p += 2;
                } else if (s[p] == '"') {
                    ++p;
                    break;
                } else {
                    ++p;
                }
            }
            t.kind = Tk::String;
            pos = p;
        } else if (isIdentStart(c)) {
            std::size_t p = pos + 1;
            while (p < s.size() && isIdentChar(s[p]))
                ++p;
            t.kind = Tk::Ident;
            pos = p;
        } else if (isDigit(c) || (c == '.' && isDigit(c1))) {
            std::size_t p = pos + 1;
            while (p < s.size()) {
                char d = s[p];
                bool exponentSign = (d == '+' || d == '-') && (s[p - 1] == 'e' || s[p - 1] == 'E');
                if (!(isDigit(d) || isAlpha(d) || d == '.' || d == '_' || exponentSign))
                    break;
                ++p;
            }
            t.kind = Tk::Number;
            pos = p;
        } else {
            static constexpr std::string_view two[] = {"->", "==", "!=", "<=", ">=", ":=", "..",
                                                       "++", "--", "**", "&&", "||", "<>"};
            std::size_t len = 0;
            for (std::string_view op : two)
                if (s.substr(pos, 2) == op)
                    len = 2;
            if (len == 0) {
                bool printablePunct = c > ' ' && c < 0x7f && !isIdentChar(c) && c != '"';
                if (!printablePunct) {
                    *stop = pos;
                    return out;
                }
                len = 1;
            }
            t.kind = Tk::Punct;
            pos += len;
        }
        t.end = pos;
        t.text = s.substr(t.begin, t.end - t.begin);
        out.push_back(t);
    }
    *stop = s.size();
    return out;
}

// 1-based line table.
class Lines {
public:
    explicit Lines(std::string_view s) : size_(s.size())
    {
        start_.push_back(0);
        for (std::size_t i = 0; i < s.size(); ++i)
            if (s[i] == '\n' && i + 1 < s.size())
                start_.push_back(i + 1);
    }
    int count() const { return static_cast<int>(start_.size()); }
    // Offset of the first byte of `line`; the text size past the last line.
    std::size_t begin(int line) const
    {
        if (line < 1)
            return 0;
        if (line > count())
            return size_;
        return start_[static_cast<std::size_t>(line) - 1];
    }
    std::size_t end(int line) const { return begin(line + 1); }  // including the newline
    int lineOf(std::size_t offset) const
    {
        auto it = std::upper_bound(start_.begin(), start_.end(), offset);
        return static_cast<int>(it - start_.begin());
    }

private:
    std::vector<std::size_t> start_;
    std::size_t size_;
};

// ---------------------------------------------------------------------------
// Model helpers
// ---------------------------------------------------------------------------

const std::set<std::string, std::less<>>& uiStateModules()
{
    // Nodes whose .net text carries UI state the model does not hold
    // (interactor values and ranges, colormaps, sequencer frames, picks, DXLink
    // ids): the legacy UI writes them through their own node classes.
    static const std::set<std::string, std::less<>> s = {
        "Integer", "IntegerList", "Scalar", "ScalarList", "Vector", "VectorList",
        "Selector", "SelectorList", "Toggle", "Value", "ValueList", "String",
        "StringList", "FileSelector", "Colormap", "Sequencer", "Reset", "Probe",
        "ProbeList", "Pick", "DXLInput", "DXLOutput"};
    return s;
}
bool isUiState(const NetNode& n) { return uiStateModules().count(n.module) != 0; }

std::string nodeName(const NetNode& n) { return n.module + "[" + std::to_string(n.instance) + "]"; }

const NetInput* findInput(const NetNode& n, int index)
{
    for (const NetInput& in : n.inputs)
        if (in.index == index)
            return &in;
    return nullptr;
}

bool sameInput(const NetInput* a, const NetInput* b)
{
    if (!a || !b)
        return a == b;
    return a->index == b->index && a->visible == b->visible && a->defaulting == b->defaulting &&
           a->type == b->type && a->value == b->value && a->hasValue == b->hasValue;
}

bool sameOutputs(const NetNode& a, const NetNode& b)
{
    if (a.outputs.size() != b.outputs.size())
        return false;
    for (std::size_t i = 0; i < a.outputs.size(); ++i) {
        const NetOutput &x = a.outputs[i], &y = b.outputs[i];
        if (x.index != y.index || x.visible != y.visible || x.type != y.type ||
            x.value != y.value || x.hasValue != y.hasValue)
            return false;
    }
    return true;
}

bool sameInputs(const NetNode& a, const NetNode& b)
{
    if (a.inputs.size() != b.inputs.size())
        return false;
    for (std::size_t i = 0; i < a.inputs.size(); ++i)
        if (!sameInput(&a.inputs[i], &b.inputs[i]))
            return false;
    return true;
}

// The DX type bit mask the legacy UI would record for a value typed in as
// text (base/DXType.h); 0 when it is not obvious.
int inferType(std::string_view v)
{
    v = trim(v);
    if (v.empty())
        return 0;
    if (v.front() == '"')
        return 0x20;                                   // string
    if (v.front() == '[')
        return 0x8;                                    // vector
    if (v.front() == '{')
        return v.find('"') != std::string_view::npos ? 0x01000020 : 0x0100001d;  // lists
    static const std::regex integer(R"([-+]?\d+)");
    static const std::regex scalar(R"([-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?)");
    std::string s(v);
    if (std::regex_match(s, integer))
        return 0x1;
    if (std::regex_match(s, scalar))
        return 0x5;
    return 0;
}

// ---------------------------------------------------------------------------
// The original text, cut into the pieces the writer moves around
// ---------------------------------------------------------------------------

// One statement: tokens [first, last], last being its ';'.
struct Stmt {
    std::size_t first = 0, last = 0;
};

// What the writer needs to know about an original node's block.
struct Block {
    int start = 0, end = 0;   // lines [start, end): the whole unit
    // Shape (only needed when the node is regenerated).
    bool shaped = true;
    std::string why;
    int stmtCount = 0;
    int stmtLine0 = 0, stmtLine1 = 0;  // first/last line of the statement
    bool call = false, expr = false;
    std::vector<std::string> lvalues;           // raw text, e.g. "main_Glyph_1_out_1[cache: 0]"
    std::map<int, std::string> outVar;          // output -> its variable name
    std::string callee, attrs;                  // attrs: "[instance: 2, cache: 1]"
    std::vector<std::string> args;              // raw argument text
    std::string lhs, rhs;                       // `lhs = rhs;` form
    // Comment part, verbatim lines (with line endings).
    std::vector<std::string> pre;               // bare lines before the node line
    std::string nodeLine;
    std::map<int, std::string> inputLines;      // first `// input[k]:` line per k
    std::vector<std::string> outputLines, otherLines;
    std::string closeLine;                      // the bare `//` ending the comments
    std::string tail;                           // lines after the statement
};

// A top-level statement or macro definition after the network macro.
struct Item {
    std::size_t begin = 0, end = 0;   // byte range, whole lines when it has them alone
    bool isMacro = false;
    std::string macroName;
    int macroParams = 0;
    int node = -1;                    // original node it assigns to
    bool input = false;
    int index = 0;                    // k of _in_k / _out_k
};

// Widens [b, e) to whole lines when only blanks share them.
void wholeLines(std::string_view text, std::size_t& b, std::size_t& e)
{
    std::size_t ls = b;
    while (ls > 0 && text[ls - 1] != '\n')
        --ls;
    if (allBlank(text.substr(ls, b - ls)))
        b = ls;
    std::size_t le = text.find('\n', e);
    le = le == std::string_view::npos ? text.size() : le + 1;
    if (allBlank(text.substr(e, le - e)))
        e = le;
}

// "<prefix><Module>_<inst>_(in|out)_<k>" -> parts. False if not of that form.
bool splitIoName(std::string_view name, std::string_view prefix, std::string& module,
                 int& instance, bool& input, int& index)
{
    if (!startsWith(name, prefix))
        return false;
    name.remove_prefix(prefix.size());
    std::size_t io = name.rfind("_in_");
    std::size_t oo = name.rfind("_out_");
    std::size_t at, len;
    if (io != std::string_view::npos && (oo == std::string_view::npos || io > oo)) {
        at = io; len = 4; input = true;
    } else if (oo != std::string_view::npos) {
        at = oo; len = 5; input = false;
    } else {
        return false;
    }
    std::string_view k = name.substr(at + len), head = name.substr(0, at);
    if (k.empty() || k.size() > 6)
        return false;
    for (char c : k)
        if (!isDigit(c))
            return false;
    std::size_t us = head.rfind('_');
    if (us == std::string_view::npos || us == 0)
        return false;
    std::string_view inst = head.substr(us + 1);
    if (inst.empty() || inst.size() > 9)
        return false;
    for (char c : inst)
        if (!isDigit(c))
            return false;
    module = std::string(head.substr(0, us));
    instance = std::stoi(std::string(inst));
    index = std::stoi(std::string(k));
    return true;
}

class Writer {
public:
    Writer(std::string_view text, const NetFile& original, const NetFile& edited,
           const ModuleLibrary* lib)
        : text_(text), lines_(text), of_(original), ef_(edited), lib_(lib)
    {
    }

    bool run(std::string& out, std::string* error);

private:
    bool refuse(std::string message)
    {
        error_ = std::move(message);
        return false;
    }

    std::string_view lineText(int line) const
    {
        return text_.substr(lines_.begin(line), lines_.end(line) - lines_.begin(line));
    }
    std::string raw(std::size_t firstTok, std::size_t lastTok) const
    {
        return std::string(text_.substr(toks_[firstTok].begin, toks_[lastTok].end - toks_[firstTok].begin));
    }
    // Comment body of a line holding only a `//` comment ("" for `// `), or
    // nullopt-like flag false when the line holds something else.
    bool commentLine(int line, std::string_view& body) const
    {
        std::string_view l = ltrim(lineText(line));
        if (!startsWith(l, "//"))
            return false;
        body = trim(l.substr(2));
        return true;
    }
    bool bareLine(int line) const
    {
        std::string_view body;
        return commentLine(line, body) && body.empty();
    }

    bool locate();
    void cutBlocks();
    void analyse(std::size_t node);
    bool scanValues();
    bool classify();
    bool order();

    std::string outVarOf(bool edited, std::size_t node, int k) const;
    std::string ownIn(const NetNode& n, int k) const
    {
        return prefix_ + n.module + "_" + std::to_string(n.instance) + "_in_" + std::to_string(k);
    }
    std::string ownOut(const NetNode& n, int k) const
    {
        return prefix_ + n.module + "_" + std::to_string(n.instance) + "_out_" + std::to_string(k);
    }
    std::string valueText(const NetNode& n, int k) const;
    const NetLink* editedLinkInto(std::size_t node, int input) const;
    const NetLink* originalLinkInto(std::size_t node, int input) const;
    bool outputConnected(std::size_t e, int k) const;

    std::string nodeLine(std::size_t e, bool patchOnly) const;
    std::string inputCommentLines(std::size_t e) const;
    bool generateBlock(std::size_t e, std::string& s, std::vector<std::string>& values);
    std::string movedBlock(std::size_t e) const;
    std::string valueGroup(std::size_t e, const std::vector<std::string>& generated) const;

    std::string_view text_;
    Lines lines_;
    const NetFile& of_;
    const NetFile& ef_;
    const ModuleLibrary* lib_;
    std::string error_;
    std::string eol_ = "\n";

    std::vector<Tok> toks_;
    std::size_t lexStop_ = 0;

    const NetMacro* om_ = nullptr;
    const NetMacro* em_ = nullptr;
    std::string prefix_;
    bool inlineValues_ = false;
    std::size_t macroTok_ = 0, openTok_ = 0, closeTok_ = 0;
    int endMarkerLine_ = 0;   // 0 when absent
    int bodyEnd_ = 0;         // line where the node units stop

    std::vector<Block> blocks_;                 // per original node
    struct Unit { int start, end; int node; };  // node -1: decorator
    std::vector<Unit> units_;
    int firstUnit_ = 0;

    std::vector<Item> items_;
    std::size_t valuesBegin_ = 0, valuesEnd_ = 0;

    // Classification.
    enum class State { Unchanged, Moved, Changed, New };
    std::vector<int> origOf_;          // edited node -> original node, -1 when new
    std::vector<int> editedOf_;        // original node -> edited node, -1 when deleted
    std::vector<State> state_;         // per edited node
    std::vector<std::set<int>> needOut_;  // per edited node: _out_k lines an unchanged node now needs
    std::vector<std::size_t> order_;   // edited nodes in write order
    std::vector<std::vector<std::string>> genValues_;  // per edited node: regenerated _in_ lines
    std::vector<std::string> genBlock_;
    bool wrapperNeeded_ = false, wrapperUpgrade_ = false;
};

// ---------------------------------------------------------------------------

bool Writer::locate()
{
    om_ = &of_.macros[0];
    em_ = &ef_.macros[0];
    std::size_t i = 0;
    for (; i < toks_.size(); ++i)
        if (toks_[i].isIdent("macro") && toks_[i].line == om_->line)
            break;
    if (i == toks_.size())
        return refuse("cannot find macro " + om_->name + " at line " + std::to_string(om_->line));
    macroTok_ = i;
    int paren = 0;
    for (++i; i < toks_.size(); ++i) {
        const Tok& t = toks_[i];
        if (t.is("(") || t.is("["))
            ++paren;
        if (t.is(")") || t.is("]"))
            --paren;
        if (paren == 0 && t.is("{"))
            break;
    }
    if (i == toks_.size())
        return refuse("cannot find the body of macro " + om_->name);
    openTok_ = i;
    int depth = 0;
    for (; i < toks_.size(); ++i) {
        const Tok& t = toks_[i];
        if (t.is("{"))
            ++depth;
        if (t.is("}") && --depth == 0)
            break;
        if (depth == 1 && t.kind == Tk::Comment && !endMarkerLine_ &&
            trim(t.text) == "network: end of macro body")
            endMarkerLine_ = t.line;
    }
    if (i == toks_.size())
        return refuse("cannot find the end of macro " + om_->name);
    closeTok_ = i;
    bodyEnd_ = endMarkerLine_ ? endMarkerLine_ : toks_[closeTok_].line;
    if (!endMarkerLine_ && !allBlank(text_.substr(lines_.begin(bodyEnd_),
                                                  toks_[closeTok_].begin - lines_.begin(bodyEnd_))))
        return refuse("macro " + om_->name + " has no end-of-body marker and its '}' is not on a line of its own");
    return true;
}

void Writer::cutBlocks()
{
    const int openLine = toks_[openTok_].line;
    std::vector<int> starts;
    blocks_.assign(om_->nodes.size(), Block{});
    std::set<int> taken;
    for (const NetNode& n : om_->nodes)
        taken.insert(n.line);
    for (const NetDecorator& d : om_->decorators)
        taken.insert(d.line);
    auto startOf = [&](int line) {
        if (line - 1 > openLine && bareLine(line - 1) && !taken.count(line - 1))
            return line - 1;
        return line;
    };
    for (std::size_t i = 0; i < om_->nodes.size(); ++i)
        units_.push_back({startOf(om_->nodes[i].line), 0, static_cast<int>(i)});
    for (const NetDecorator& d : om_->decorators)
        if (d.line > openLine && d.line < bodyEnd_)
            units_.push_back({startOf(d.line), 0, -1});
    std::sort(units_.begin(), units_.end(), [](const Unit& a, const Unit& b) { return a.start < b.start; });
    for (std::size_t u = 0; u < units_.size(); ++u)
        units_[u].end = u + 1 < units_.size() ? units_[u + 1].start : bodyEnd_;
    firstUnit_ = units_.empty() ? bodyEnd_ : units_.front().start;
    for (const Unit& u : units_)
        if (u.node >= 0) {
            blocks_[static_cast<std::size_t>(u.node)].start = u.start;
            blocks_[static_cast<std::size_t>(u.node)].end = u.end;
        }
}

// Statements and comment lines of one node's block; see Block.
void Writer::analyse(std::size_t node)
{
    Block& b = blocks_[node];
    const NetNode& n = om_->nodes[node];
    const std::size_t from = lines_.begin(b.start), to = lines_.begin(b.end);
    auto unshaped = [&](std::string why) {
        if (b.shaped) {
            b.shaped = false;
            b.why = std::move(why);
        }
    };

    // Statements in the block.
    std::vector<Stmt> stmts;
    std::size_t i = std::lower_bound(toks_.begin(), toks_.end(), from,
                                     [](const Tok& t, std::size_t off) { return t.begin < off; }) -
                    toks_.begin();
    int depth = 0;
    std::size_t first = 0;
    bool inStmt = false;
    for (; i < toks_.size() && toks_[i].begin < to; ++i) {
        const Tok& t = toks_[i];
        if (t.kind == Tk::Comment)
            continue;
        if (!inStmt) {
            first = i;
            inStmt = true;
        }
        if (t.is("(") || t.is("[") || t.is("{"))
            ++depth;
        if (t.is(")") || t.is("]") || t.is("}"))
            --depth;
        if (depth == 0 && t.is(";")) {
            stmts.push_back({first, i});
            inStmt = false;
        }
    }
    if (inStmt)
        unshaped("a statement runs past the block");
    b.stmtCount = static_cast<int>(stmts.size());
    if (stmts.size() > 1)
        unshaped("more than one statement");

    // Output variables of every statement (a Sequencer has two).
    for (const Stmt& st : stmts) {
        int d = 0;
        bool lhs = false;
        for (std::size_t k = st.first; k < st.last; ++k)
            if (toks_[k].is("=") && d == 0) { lhs = true; break; }
            else if (toks_[k].is("(") || toks_[k].is("[")) ++d;
            else if (toks_[k].is(")") || toks_[k].is("]")) --d;
        if (!lhs)
            continue;
        d = 0;
        for (std::size_t k = st.first; k < st.last && !toks_[k].is("="); ++k) {
            const Tok& t = toks_[k];
            if (t.is("[")) ++d;
            if (t.is("]")) --d;
            if (d != 0 || t.kind != Tk::Ident)
                continue;
            std::size_t pos = t.text.rfind("_out_");
            if (pos == std::string_view::npos)
                continue;
            std::string_view digits = t.text.substr(pos + 5);
            bool ok = !digits.empty() && digits.size() < 7;
            for (char c : digits) ok = ok && isDigit(c);
            if (ok)
                b.outVar[std::stoi(std::string(digits))] = std::string(t.text);
        }
    }

    int commentEnd = b.end;   // lines [start, commentEnd) are the comment part
    if (stmts.size() == 1) {
        const Stmt& s = stmts[0];
        b.stmtLine0 = toks_[s.first].line;
        b.stmtLine1 = toks_[s.last].line;
        commentEnd = b.stmtLine0;
        if (!allBlank(text_.substr(lines_.begin(b.stmtLine0), toks_[s.first].begin - lines_.begin(b.stmtLine0))) ||
            !allBlank(text_.substr(toks_[s.last].end, lines_.end(b.stmtLine1) - toks_[s.last].end)))
            unshaped("the statement shares a line with other text");
        for (int l = b.stmtLine1 + 1; l < b.end; ++l)
            b.tail += lineText(l);

        // lvalues = callee(args) [attrs];
        std::size_t eq = s.last;
        depth = 0;
        for (std::size_t k = s.first; k < s.last; ++k) {
            const Tok& t = toks_[k];
            if (t.is("(") || t.is("[") || t.is("{")) ++depth;
            if (t.is(")") || t.is("]") || t.is("}")) --depth;
            if (depth == 0 && t.is("=")) {
                eq = k;
                break;
            }
        }
        std::size_t rhs = s.first;
        if (eq != s.last) {
            if (eq == s.first)
                unshaped("statement starts with '='");
            std::size_t g = s.first;
            depth = 0;
            for (std::size_t k = s.first; k <= eq; ++k) {
                const Tok& t = toks_[k];
                if (t.is("[")) ++depth;
                if (t.is("]")) --depth;
                if (k == eq || (depth == 0 && t.is(","))) {
                    if (k == g) {
                        unshaped("empty output variable");
                        break;
                    }
                    b.lvalues.push_back(raw(g, k - 1));
                    const Tok& name = toks_[g];
                    if (name.kind == Tk::Ident) {
                        std::size_t pos = name.text.rfind("_out_");
                        if (pos != std::string_view::npos) {
                            std::string_view digits = name.text.substr(pos + 5);
                            bool ok = !digits.empty() && digits.size() < 7;
                            for (char c : digits) ok = ok && isDigit(c);
                            if (ok)
                                b.outVar[std::stoi(std::string(digits))] = std::string(name.text);
                        }
                    }
                    g = k + 1;
                }
            }
            rhs = eq + 1;
        }
        if (rhs + 1 < s.last && toks_[rhs].kind == Tk::Ident && toks_[rhs + 1].is("(")) {
            b.call = true;
            b.callee = std::string(toks_[rhs].text);
            std::size_t k = rhs + 2, a = k;
            depth = 0;
            bool closed = false;
            for (; k < s.last; ++k) {
                const Tok& t = toks_[k];
                if (t.is("(") || t.is("[") || t.is("{")) ++depth;
                if (t.is(")") || t.is("]") || t.is("}")) {
                    if (depth == 0) {
                        if (k > a || !b.args.empty())
                            b.args.push_back(k > a ? raw(a, k - 1) : std::string());
                        closed = true;
                        break;
                    }
                    --depth;
                }
                if (depth == 0 && t.is(",")) {
                    b.args.push_back(k > a ? raw(a, k - 1) : std::string());
                    a = k + 1;
                }
            }
            if (!closed) {
                unshaped("unbalanced call");
            } else if (k + 1 < s.last) {
                if (!toks_[k + 1].is("[") || !toks_[s.last - 1].is("]"))
                    unshaped("unexpected text after the call");
                b.attrs = raw(k + 1, s.last - 1);
            }
        } else if (eq != s.last && rhs < s.last) {
            b.expr = true;
            b.lhs = raw(s.first, eq - 1);
            b.rhs = raw(rhs, s.last - 1);
        } else {
            unshaped("statement is neither a call nor an assignment");
        }
    } else if (stmts.empty()) {
        // Comment part ends after the last comment line; the rest is tail.
        int last = b.start;
        for (int l = b.start; l < b.end; ++l)
            if (!allBlank(lineText(l)))
                last = l;
        commentEnd = last + 1;
        for (int l = commentEnd; l < b.end; ++l)
            b.tail += lineText(l);
    }

    // Comment part.
    bool seenNode = false;
    for (int l = b.start; l < commentEnd; ++l) {
        std::string_view body;
        std::string line(lineText(l));
        if (!commentLine(l, body)) {
            if (allBlank(line)) {
                b.otherLines.push_back(line);
                continue;
            }
            unshaped("non-comment text among the node's comments");
            continue;
        }
        if (l == n.line) {
            b.nodeLine = line;
            seenNode = true;
        } else if (!seenNode) {
            if (body.empty())
                b.pre.push_back(line);
            else
                unshaped("comment before the node line");
        } else if (body.empty() && l == commentEnd - 1) {
            b.closeLine = line;
        } else if (startsWith(body, "input[")) {
            std::size_t rb = body.find(']');
            int k = 0;
            bool ok = rb != std::string_view::npos && rb > 6 && rb < 14;
            for (std::size_t c = 6; ok && c < rb; ++c) ok = isDigit(body[c]);
            if (ok)
                k = std::stoi(std::string(body.substr(6, rb - 6)));
            if (k > 0 && !b.inputLines.count(k))
                b.inputLines[k] = line;
            else
                b.otherLines.push_back(line);
        } else if (startsWith(body, "output[")) {
            b.outputLines.push_back(line);
        } else {
            b.otherLines.push_back(line);
        }
    }
    if (!seenNode)
        unshaped("node comment not found in its block");
}

bool Writer::scanValues()
{
    valuesBegin_ = toks_[closeTok_].begin;
    valuesEnd_ = lexStop_ < text_.size() ? lines_.begin(lines_.lineOf(lexStop_)) : text_.size();
    std::map<std::pair<std::string, int>, int> byName;
    for (std::size_t i = 0; i < om_->nodes.size(); ++i)
        byName[{om_->nodes[i].module, om_->nodes[i].instance}] = static_cast<int>(i);

    std::size_t i = closeTok_ + 1;
    while (i < toks_.size() && toks_[i].end <= valuesEnd_) {
        const Tok& t = toks_[i];
        if (t.kind == Tk::Comment) {
            ++i;
            continue;
        }
        if (t.isIdent("include") && i + 1 < toks_.size() && toks_[i + 1].kind == Tk::String) {
            i += 2;
            continue;
        }
        Item item;
        if (t.isIdent("macro")) {
            item.isMacro = true;
            std::size_t k = i + 1;
            if (k < toks_.size() && toks_[k].kind == Tk::Ident)
                item.macroName = std::string(toks_[k++].text);
            // Parameters: depth-0 commas inside the first (...), plus one.
            int depth = 0, params = 0;
            bool any = false;
            if (k < toks_.size() && toks_[k].is("(")) {
                for (++k; k < toks_.size(); ++k) {
                    const Tok& u = toks_[k];
                    if (u.kind == Tk::Comment) continue;
                    if (u.is("(") || u.is("[") || u.is("{")) ++depth;
                    if (u.is(")") || u.is("]") || u.is("}")) {
                        if (depth == 0) break;
                        --depth;
                    }
                    if (depth == 0 && u.is(",")) ++params;
                    any = true;
                }
                if (any) ++params;
            }
            while (k < toks_.size() && !toks_[k].is("{")) ++k;
            item.macroParams = params;
            depth = 0;
            for (; k < toks_.size(); ++k) {
                if (toks_[k].is("{")) ++depth;
                if (toks_[k].is("}") && --depth == 0) break;
            }
            if (k == toks_.size())
                break;   // damaged; leave the rest as it is
            item.begin = t.begin;
            item.end = toks_[k].end;
            wholeLines(text_, item.begin, item.end);
            items_.push_back(item);
            i = k + 1;
            continue;
        }
        // A statement up to ';' at depth 0.
        std::size_t k = i;
        int depth = 0;
        for (; k < toks_.size(); ++k) {
            const Tok& u = toks_[k];
            if (u.kind == Tk::Comment) break;
            if (u.is("(") || u.is("[") || u.is("{")) ++depth;
            if (u.is(")") || u.is("]") || u.is("}")) --depth;
            if (depth == 0 && u.is(";")) break;
        }
        if (k == toks_.size() || !toks_[k].is(";")) {
            ++i;   // not a statement we understand; leave it in the gap text
            continue;
        }
        item.begin = t.begin;
        item.end = toks_[k].end;
        wholeLines(text_, item.begin, item.end);
        if (t.kind == Tk::Ident && i + 1 < k && toks_[i + 1].is("=")) {
            std::string module;
            int inst = 0, index = 0;
            bool input = false;
            if (splitIoName(t.text, prefix_, module, inst, input, index)) {
                auto it = byName.find({module, inst});
                if (it != byName.end()) {
                    item.node = it->second;
                    item.input = input;
                    item.index = index;
                }
            }
        }
        if (item.node >= 0 || item.isMacro)
            items_.push_back(item);
        i = k + 1;
    }
    return true;
}

const NetLink* Writer::editedLinkInto(std::size_t node, int input) const
{
    for (const NetLink& l : em_->links)
        if (l.toNode == node && l.toInput == input)
            return &l;
    return nullptr;
}

const NetLink* Writer::originalLinkInto(std::size_t node, int input) const
{
    for (const NetLink& l : om_->links)
        if (l.toNode == node && l.toInput == input)
            return &l;
    return nullptr;
}

bool Writer::outputConnected(std::size_t e, int k) const
{
    for (const NetLink& l : em_->links)
        if (l.fromNode == e && l.fromOutput == k)
            return true;
    if (k == 1 && em_->nodes[e].module == "Transmitter")
        for (const NetWireless& w : em_->wirelessPairs())
            if (w.transmitter == e)
                return true;
    return false;
}

// The variable carrying output k of a node: what its original statement
// assigns, else the legacy name.
std::string Writer::outVarOf(bool edited, std::size_t node, int k) const
{
    int o = edited ? origOf_[node] : static_cast<int>(node);
    if (o >= 0) {
        const Block& b = blocks_[static_cast<std::size_t>(o)];
        auto it = b.outVar.find(k);
        if (it != b.outVar.end())
            return it->second;
    }
    const NetNode& n = edited ? em_->nodes[node] : om_->nodes[node];
    return ownOut(n, k);
}

// The value the legacy UI assigns to an unconnected input: its set value,
// NULL when it defaults.
std::string Writer::valueText(const NetNode& n, int k) const
{
    const NetInput* in = findInput(n, k);
    if (in && in->hasValue && !in->defaulting)
        return in->value;
    return "NULL";
}

bool Writer::classify()
{
    const std::size_t ne = em_->nodes.size(), no = om_->nodes.size();
    origOf_.assign(ne, -1);
    editedOf_.assign(no, -1);
    state_.assign(ne, State::New);
    needOut_.assign(ne, {});
    genValues_.assign(ne, {});
    genBlock_.assign(ne, {});
    for (std::size_t e = 0; e < ne; ++e) {
        const NetNode& n = em_->nodes[e];
        if (n.line == 0)
            continue;
        for (std::size_t o = 0; o < no; ++o)
            if (om_->nodes[o].line == n.line && om_->nodes[o].module == n.module &&
                om_->nodes[o].instance == n.instance) {
                if (editedOf_[o] >= 0)
                    return refuse(nodeName(n) + " appears twice in the edited network");
                origOf_[e] = static_cast<int>(o);
                editedOf_[o] = static_cast<int>(e);
                break;
            }
        if (origOf_[e] < 0)
            return refuse(nodeName(n) + ": no node of that name at line " + std::to_string(n.line) +
                          " in the original");
    }
    // Two live nodes with one name would share variables.
    std::set<std::pair<std::string, int>> names;
    for (const NetNode& n : em_->nodes)
        if (!names.insert({n.module, n.instance}).second)
            return refuse("two nodes are named " + nodeName(n));

    for (std::size_t o = 0; o < no; ++o) {
        if (editedOf_[o] >= 0)
            continue;
        const NetNode& n = om_->nodes[o];
        if (n.module == "Input" || n.module == "Output")
            return refuse("deleting " + nodeName(n) + " would change the macro's parameters; not supported");
    }

    for (std::size_t e = 0; e < ne; ++e) {
        const NetNode& n = em_->nodes[e];
        if (origOf_[e] < 0) {
            if (isUiState(n))
                return refuse("adding " + nodeName(n) + ": " + n.module +
                              " nodes carry UI state the writer cannot produce yet");
            if (n.module == "Transmitter" || n.module == "Receiver")
                return refuse("adding " + nodeName(n) + ": Transmitter/Receiver need a label; not supported yet");
            if (n.module == "Input" || n.module == "Output")
                return refuse("adding " + nodeName(n) + " would change the macro's parameters; not supported");
            continue;
        }
        const auto o = static_cast<std::size_t>(origOf_[e]);
        const NetNode& m = om_->nodes[o];
        bool wires = true;
        int maxIn = std::max(n.numInputs, m.numInputs);
        for (const NetLink& l : om_->links)
            maxIn = std::max(maxIn, l.toNode == o ? l.toInput : 0);
        for (const NetLink& l : em_->links)
            maxIn = std::max(maxIn, l.toNode == e ? l.toInput : 0);
        for (int k = 1; k <= maxIn && wires; ++k) {
            const NetLink* a = originalLinkInto(o, k);
            const NetLink* b = editedLinkInto(e, k);
            if (!a || !b) {
                wires = !a && !b;
                continue;
            }
            const NetNode& sa = om_->nodes[a->fromNode];
            const NetNode& sb = em_->nodes[b->fromNode];
            wires = a->fromOutput == b->fromOutput && sa.module == sb.module && sa.instance == sb.instance &&
                    outVarOf(false, a->fromNode, a->fromOutput) == outVarOf(true, b->fromNode, b->fromOutput);
        }
        bool same = wires && sameInputs(n, m) && n.numInputs == m.numInputs && n.label == m.label &&
                    sameOutputs(n, m) && n.extraComments == m.extraComments &&
                    (n.numOutputs == m.numOutputs || !m.outputsFromComment);
        if (!same) {
            if (isUiState(n))
                return refuse("changing the inputs of " + nodeName(n) + ": " + n.module +
                              " nodes carry UI state the writer cannot produce yet");
            state_[e] = State::Changed;
        } else if (std::lround(n.x) != std::lround(m.x) || std::lround(n.y) != std::lround(m.y)) {
            state_[e] = State::Moved;
        } else {
            state_[e] = State::Unchanged;
        }
    }

    // A wire from an output the source's text does not assign: regenerate the
    // source, or -- for interactors, assigned only in the values section --
    // give it an `_out_k = value;` line.
    for (const NetLink& l : em_->links) {
        const std::size_t e = l.fromNode;
        if (origOf_[e] < 0 || state_[e] == State::Changed)
            continue;
        const auto o = static_cast<std::size_t>(origOf_[e]);
        const Block& b = blocks_[o];
        if (b.outVar.count(l.fromOutput))
            continue;
        // Wired already in the original: its text provides the value, or
        // provided nothing and that worked.
        bool wasWired = false;
        for (const NetLink& ol : om_->links)
            wasWired = wasWired || (ol.fromNode == o && ol.fromOutput == l.fromOutput);
        if (wasWired)
            continue;
        if (b.stmtCount == 0) {
            needOut_[e].insert(l.fromOutput);
        } else if (b.call) {
            state_[e] = State::Changed;
        } else {
            return refuse("output " + std::to_string(l.fromOutput) + " of " + nodeName(em_->nodes[e]) +
                          " is not assigned in its text");
        }
    }
    // Drop needOut entries the values section already has.
    for (const Item& it : items_)
        if (it.node >= 0 && !it.input && editedOf_[static_cast<std::size_t>(it.node)] >= 0)
            needOut_[static_cast<std::size_t>(editedOf_[static_cast<std::size_t>(it.node)])].erase(it.index);

    for (std::size_t e = 0; e < ne; ++e) {
        if (state_[e] != State::Changed || origOf_[e] < 0)
            continue;
        if (isUiState(em_->nodes[e]))
            return refuse("changing " + nodeName(em_->nodes[e]) + ": " + em_->nodes[e].module +
                          " nodes carry UI state the writer cannot produce yet");
        const auto o = static_cast<std::size_t>(origOf_[e]);
        const Block& b = blocks_[o];
        if (!b.shaped)
            return refuse("cannot rewrite " + nodeName(em_->nodes[e]) + " (line " +
                          std::to_string(em_->nodes[e].line) + "): " + b.why);
        if (b.stmtCount == 0)
            return refuse("cannot rewrite " + nodeName(em_->nodes[e]) + ": it has no statement to rewrite");
        if (b.expr && em_->nodes[e].numInputs > 1)
            return refuse("cannot rewrite " + nodeName(em_->nodes[e]) + ": assignment with several inputs");
    }
    return true;
}

// Each node after the nodes it reads (wires, Transmitter -> Receiver), keeping
// the original order wherever it is valid. Dependencies the original order
// already broke are left as they were.
bool Writer::order()
{
    const std::size_t ne = em_->nodes.size();
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    for (const NetLink& l : em_->links)
        edges.push_back({l.fromNode, l.toNode});
    for (const NetWireless& w : em_->wirelessPairs())
        edges.push_back({w.transmitter, w.receiver});

    std::set<std::pair<int, int>> tolerated;   // original (from, to) out of order
    for (const NetLink& l : om_->links)
        if (l.fromNode > l.toNode)
            tolerated.insert({static_cast<int>(l.fromNode), static_cast<int>(l.toNode)});
    for (const NetWireless& w : om_->wirelessPairs())
        if (w.transmitter > w.receiver)
            tolerated.insert({static_cast<int>(w.transmitter), static_cast<int>(w.receiver)});

    std::vector<std::size_t> rank(ne);
    for (std::size_t e = 0; e < ne; ++e)
        rank[e] = origOf_[e] >= 0 ? static_cast<std::size_t>(origOf_[e]) : om_->nodes.size() + e;
    std::vector<std::vector<std::size_t>> succ(ne);
    std::vector<int> indeg(ne, 0);
    for (auto [f, t] : edges) {
        if (f == t)
            continue;
        if (origOf_[f] >= 0 && origOf_[t] >= 0 && tolerated.count({origOf_[f], origOf_[t]}))
            continue;
        succ[f].push_back(t);
        ++indeg[t];
    }
    // Plain topological order first, to find each node's earliest consumer.
    std::vector<std::size_t> topo;
    {
        std::vector<int> deg = indeg;
        std::vector<std::size_t> todo;
        for (std::size_t e = 0; e < ne; ++e)
            if (deg[e] == 0)
                todo.push_back(e);
        while (!todo.empty()) {
            std::size_t e = todo.back();
            todo.pop_back();
            topo.push_back(e);
            for (std::size_t t : succ[e])
                if (--deg[t] == 0)
                    todo.push_back(t);
        }
        if (topo.size() != ne)
            return refuse("the network has a cycle");
    }
    // key = (lowest rank among the node and everything downstream, own rank):
    // a node goes just before the first node that needs it, an unneeded new
    // node after the original ones, and a valid original order stays as it is.
    std::vector<std::size_t> low(rank);
    for (auto it = topo.rbegin(); it != topo.rend(); ++it)
        for (std::size_t t : succ[*it])
            low[*it] = std::min(low[*it], low[t]);
    auto cmp = [&](std::size_t a, std::size_t b) {
        return low[a] != low[b] ? low[a] > low[b] : rank[a] > rank[b];
    };
    std::vector<std::size_t> ready;
    for (std::size_t e = 0; e < ne; ++e)
        if (indeg[e] == 0)
            ready.push_back(e);
    std::make_heap(ready.begin(), ready.end(), cmp);
    while (!ready.empty()) {
        std::pop_heap(ready.begin(), ready.end(), cmp);
        std::size_t e = ready.back();
        ready.pop_back();
        order_.push_back(e);
        for (std::size_t t : succ[e])
            if (--indeg[t] == 0) {
                ready.push_back(t);
                std::push_heap(ready.begin(), ready.end(), cmp);
            }
    }
    if (order_.size() != ne)
        return refuse("the network has a cycle");
    return true;
}

// `// node Mod[inst]: x = X, y = Y, inputs = N[, outputs = M], label = L`.
std::string Writer::nodeLine(std::size_t e, bool patchOnly) const
{
    const NetNode& n = em_->nodes[e];
    const long x = std::lround(n.x), y = std::lround(n.y);
    const int o = origOf_[e];
    if (o >= 0) {
        const std::string& line = blocks_[static_cast<std::size_t>(o)].nodeLine;
        const NetNode& m = om_->nodes[static_cast<std::size_t>(o)];
        static const std::regex xy(R"(^(\s*//\s*node\s+\w+\[\d+\]:\s*x\s*=\s*)(-?\d+)(\s*,\s*y\s*=\s*)(-?\d+))");
        std::smatch mt;
        bool sameRest = n.label == m.label && n.numInputs == m.numInputs &&
                        (n.numOutputs == m.numOutputs || !m.outputsFromComment);
        if ((patchOnly || sameRest) && std::regex_search(line, mt, xy))
            return mt[1].str() + std::to_string(x) + mt[3].str() + std::to_string(y) + mt.suffix().str();
    }
    bool showOutputs = false;
    if (o >= 0) {
        showOutputs = om_->nodes[static_cast<std::size_t>(o)].outputsFromComment;
    } else if (lib_) {
        const ModuleDef* def = lib_->find(n.module);
        showOutputs = def && static_cast<int>(def->outputs.size()) != n.numOutputs;
    }
    std::string s = "    // node " + n.module + "[" + std::to_string(n.instance) + "]: x = " +
                    std::to_string(x) + ", y = " + std::to_string(y) +
                    ", inputs = " + std::to_string(n.numInputs);
    if (showOutputs)
        s += ", outputs = " + std::to_string(n.numOutputs);
    s += ", label = " + n.label + eol_;
    return s;
}

// The block of a node that only moved: its text with x/y patched.
std::string Writer::movedBlock(std::size_t e) const
{
    const auto o = static_cast<std::size_t>(origOf_[e]);
    const Block& b = blocks_[o];
    const int nl = om_->nodes[o].line;
    std::string s;
    for (int l = b.start; l < b.end; ++l) {
        if (l == nl) {
            static const std::regex xy(R"(^(\s*//\s*node\s+\w+\[\d+\]:\s*x\s*=\s*)(-?\d+)(\s*,\s*y\s*=\s*)(-?\d+))");
            std::string line(lineText(l));
            std::smatch mt;
            const NetNode& n = em_->nodes[e];
            if (std::regex_search(line, mt, xy))
                line = mt[1].str() + std::to_string(std::lround(n.x)) + mt[3].str() +
                       std::to_string(std::lround(n.y)) + mt.suffix().str();
            s += line;
        } else {
            s += lineText(l);
        }
    }
    return s;
}

// A node's block in the legacy format (Node::netPrintCommentHeader and
// netNodeString), reusing the original's lines where they still hold.
// `values` receives the node's regenerated `_in_k = ...;` lines.
bool Writer::generateBlock(std::size_t e, std::string& s, std::vector<std::string>& values)
{
    NetNode n = em_->nodes[e];
    const int oi = origOf_[e];
    const Block* b = oi >= 0 ? &blocks_[static_cast<std::size_t>(oi)] : nullptr;
    const NetNode* m = oi >= 0 ? &om_->nodes[static_cast<std::size_t>(oi)] : nullptr;
    const bool isImage = n.module == "Image";
    const bool isDisplay = isImage || n.module == "Display";

    // A new Image gets what ImageNode::initialize sets: its window id, and
    // useVector 0, buttonState 1, renderMode 0 (the wrapper's camera switch
    // needs useVector).
    if (!b && isImage) {
        struct Init { int index; int type; std::string value; };
        const Init init[] = {{1, 0x20, "\"Image_" + std::to_string(n.instance) + "\""},
                             {4, 0x1, "0"}, {14, 0x1, "1"}, {19, 0x3, "0"}};
        for (const Init& i : init) {
            if (findInput(n, i.index))
                continue;
            NetInput in;
            in.index = i.index;
            in.visible = false;
            in.defaulting = false;
            in.type = i.type;
            in.value = i.value;
            in.hasValue = true;
            n.inputs.push_back(in);
            n.numInputs = std::max(n.numInputs, i.index);
        }
    }

    // Comment part.
    if (b && !b->pre.empty())
        for (const std::string& l : b->pre) s += l;
    else
        s += "    // " + eol_;
    s += nodeLine(e, false);
    std::vector<NetInput> ins = n.inputs;
    std::stable_sort(ins.begin(), ins.end(), [](const NetInput& a, const NetInput& c) { return a.index < c.index; });
    for (const NetInput& in : ins) {
        // Reuse the line unless the input became wired: a wired input's value
        // is written as defaulting (Node::printIOComment).
        const bool newlyWired = editedLinkInto(e, in.index) && !originalLinkInto(static_cast<std::size_t>(oi), in.index);
        if (b && m && sameInput(&in, findInput(*m, in.index)) && b->inputLines.count(in.index) &&
            !(newlyWired && in.hasValue && !in.defaulting)) {
            s += b->inputLines.at(in.index);
            continue;
        }
        if (isImage && in.index == 24)
            continue;   // RECENABLE: ImageNode::printIOComment skips it
        if (in.hasValue) {
            int type = in.type ? in.type : inferType(in.value);
            s += "    // input[" + std::to_string(in.index) + "]: defaulting = " +
                 (in.defaulting || editedLinkInto(e, in.index) ? "1" : "0") +
                 ", visible = " + (in.visible ? "1" : "0") + ", type = " + std::to_string(type) +
                 ", value = " + in.value + eol_;
        } else {
            s += "    // input[" + std::to_string(in.index) + "]: visible = " + (in.visible ? "1" : "0") + eol_;
        }
    }
    if (b && m && sameOutputs(n, *m)) {
        for (const std::string& l : b->outputLines) s += l;
    } else {
        for (const NetOutput& out : n.outputs) {
            if (out.hasValue)
                s += "    // output[" + std::to_string(out.index) + "]: visible = " + (out.visible ? "1" : "0") +
                     ", type = " + std::to_string(out.type) + ", value = " + out.value + eol_;
            else
                s += "    // output[" + std::to_string(out.index) + "]: visible = " +
                     (out.visible ? "1" : "0") + eol_;
        }
    }
    if (b && m && n.extraComments == m->extraComments) {
        for (const std::string& l : b->otherLines) s += l;
    } else if (!b && n.module == "Compute" && n.extraComments.empty()) {
        // ComputeNode names its inputs a, b, ... (ComputeNode::netPrintAuxComment).
        for (int k = 2; k <= n.numInputs && k < 28; ++k)
            s += "    // name[" + std::to_string(k) + "]: value = " + std::string(1, static_cast<char>('a' + k - 2)) + eol_;
    } else if (!b && isDisplay && n.extraComments.empty()) {
        // DisplayNode/ImageNode::printCommonComments for a node with no window yet.
        s += "    // depth: value = 24" + eol_;
        if (isImage)
            s += "    // internal caching: 1" + eol_;
    } else {
        for (const std::string& c : n.extraComments)
            s += "    // " + c + eol_;
    }
    s += b && !b->closeLine.empty() ? b->closeLine : "    //" + eol_;

    // Arguments.
    auto argFor = [&](int k) -> std::string {
        if (const NetLink* l = editedLinkInto(e, k))
            return outVarOf(true, l->fromNode, l->fromOutput);
        const auto o = static_cast<std::size_t>(oi);
        const bool haveArg = b && b->call && k <= static_cast<int>(b->args.size());
        const bool same = m && sameInput(findInput(n, k), findInput(*m, k)) && !originalLinkInto(o, k);
        if (b && b->expr && k == 1 && same)
            return b->rhs;
        if (haveArg && same) {
            const std::string& a = b->args[static_cast<std::size_t>(k) - 1];
            if (a == ownIn(n, k)) {
                for (const Item& it : items_)
                    if (it.node == oi && it.input && it.index == k) {
                        values.push_back(std::string(text_.substr(it.begin, it.end - it.begin)));
                        break;
                    }
            }
            return a;
        }
        std::string v = valueText(n, k);
        if (isDisplay && k == 3 && v == "NULL")
            v = "\"X24,,\"";   // DisplayNode::inputValueString: depth 24, no title
        if (inlineValues_)
            return v;
        values.push_back(ownIn(n, k) + " = " + v + ";" + eol_);
        return ownIn(n, k);
    };

    if (b && b->expr) {
        s += b->lhs + " = " + argFor(1) + ";" + eol_;
        s += b->tail;
        return true;
    }

    std::vector<std::string> outs;
    const int numOut = n.numOutputs;
    for (int k = 1; k <= numOut; ++k) {
        std::string v;
        if (b) {
            // Reuse the lvalue text (keeps `[cache: 0]`).
            for (const std::string& lv : b->lvalues) {
                std::string_view name = trim(std::string_view(lv).substr(0, lv.find('[')));
                if (name == outVarOf(true, e, k)) {
                    v = lv;
                    break;
                }
            }
        }
        outs.push_back(v.empty() ? outVarOf(true, e, k) : v);
    }
    std::string args;
    for (int k = 1; k <= n.numInputs; ++k) {
        args += "    " + argFor(k);
        if (k != n.numInputs)
            args += "," + eol_;
    }
    const std::string callee = b ? b->callee : n.module;
    const std::string attrs = b && !b->attrs.empty()
                                  ? b->attrs
                                  : "[instance: " + std::to_string(n.instance) + ", cache: 1]";
    if (!outs.empty()) {
        for (std::size_t i = 0; i < outs.size(); ++i)
            s += outs[i] + (i + 1 < outs.size() ? "," + eol_ : "");
        s += " = " + eol_;
    }
    s += "    " + callee + "(" + eol_ + args + eol_ + "    ) " + attrs + ";" + eol_;
    if (b)
        s += b->tail;
    return true;
}

// A changed or new node's value assignments: regenerated inputs (from
// generateBlock), then `_out_k = NULL;` for connected outputs, original lines
// reused verbatim.
std::string Writer::valueGroup(std::size_t e, const std::vector<std::string>& generated) const
{
    std::string s;
    for (const std::string& v : generated)
        s += v;
    const NetNode& n = em_->nodes[e];
    const int oi = origOf_[e];
    for (int k = 1; k <= n.numOutputs; ++k) {
        bool found = false;
        if (oi >= 0)
            for (const Item& it : items_)
                if (it.node == oi && !it.input && it.index == k) {
                    s += text_.substr(it.begin, it.end - it.begin);
                    found = true;
                    break;
                }
        if (!found && outputConnected(e, k))
            s += ownOut(n, k) + " = NULL;" + eol_;
    }
    return s;
}

bool Writer::run(std::string& out, std::string* error)
{
    auto bail = [&]() {
        fail(error, error_);
        return false;
    };
    if (of_.macros.size() != 1 || ef_.macros.size() != 1) {
        error_ = "only files with exactly one network macro are supported";
        return bail();
    }
    {
        const NetMacro &a = of_.macros[0], &b = ef_.macros[0];
        bool same = a.name == b.name && a.comments == b.comments && a.workspaceWidth == b.workspaceWidth &&
                    a.workspaceHeight == b.workspaceHeight && a.category == b.category &&
                    a.description == b.description && a.headerComments == b.headerComments &&
                    a.formalInputs.size() == b.formalInputs.size() &&
                    a.formalOutputs.size() == b.formalOutputs.size() &&
                    a.decorators.size() == b.decorators.size() && a.line == b.line;
        for (std::size_t i = 0; same && i < a.formalInputs.size(); ++i)
            same = a.formalInputs[i].name == b.formalInputs[i].name &&
                   a.formalInputs[i].defaultValue == b.formalInputs[i].defaultValue;
        for (std::size_t i = 0; same && i < a.formalOutputs.size(); ++i)
            same = a.formalOutputs[i].name == b.formalOutputs[i].name;
        for (std::size_t i = 0; same && i < a.decorators.size(); ++i)
            same = a.decorators[i].comments == b.decorators[i].comments;
        if (!same) {
            error_ = "macro-level changes (name, parameters, comments, workspace, decorators) are not supported";
            return bail();
        }
    }
    for (const NetMacro* m : {&of_.macros[0], &ef_.macros[0]})
        for (const NetLink& l : m->links)
            if (l.fromNode >= m->nodes.size() || l.toNode >= m->nodes.size()) {
                error_ = "a wire refers to a node that does not exist";
                return bail();
            }

    std::size_t nl = text_.find('\n');
    if (nl != std::string_view::npos && nl > 0 && text_[nl - 1] == '\r')
        eol_ = "\r\n";
    toks_ = tokenize(text_, &lexStop_);
    if (!locate())
        return bail();

    // Variable naming: `<macro>_Mod_inst_...` since 2.0, `Mod_inst_...` before.
    prefix_ = om_->name + "_";
    cutBlocks();
    for (std::size_t i = 0; i < om_->nodes.size(); ++i)
        analyse(i);
    {
        int with = 0, without = 0;
        for (std::size_t i = 0; i < om_->nodes.size(); ++i)
            for (const auto& [k, v] : blocks_[i].outVar) {
                (void)k;
                const NetNode& n = om_->nodes[i];
                if (startsWith(v, prefix_ + n.module + "_")) ++with;
                else if (startsWith(v, n.module + "_")) ++without;
            }
        if (without > with)
            prefix_.clear();
    }
    // Input values as `_in_k` variables (main), or inline (macros).
    {
        bool vars = false, constants = false;
        for (std::size_t i = 0; i < om_->nodes.size(); ++i) {
            const Block& b = blocks_[i];
            for (std::size_t k = 0; k < b.args.size(); ++k) {
                if (b.args[k] == ownIn(om_->nodes[i], static_cast<int>(k) + 1)) vars = true;
                else if (!originalLinkInto(i, static_cast<int>(k) + 1)) constants = true;
            }
        }
        inlineValues_ = vars ? false : constants ? true : om_->name != "main";
    }
    if (!scanValues())
        return bail();
    if (!classify() || !order())
        return bail();

    // Image wrapper.
    int wrapperParams = -1;
    for (const Item& it : items_)
        if (it.isMacro && it.macroName == "Image")
            wrapperParams = std::max(wrapperParams, it.macroParams);
    int imageInputs = 0;
    bool newImage = false;
    for (std::size_t e = 0; e < em_->nodes.size(); ++e)
        if (em_->nodes[e].module == "Image" && state_[e] != State::Unchanged && state_[e] != State::Moved) {
            imageInputs = std::max(imageInputs, em_->nodes[e].numInputs);
            newImage = newImage || state_[e] == State::New;
        }
    wrapperNeeded_ = newImage && wrapperParams < 0;
    wrapperUpgrade_ = wrapperParams >= 0 && imageInputs > wrapperParams;

    // Blocks of changed and new nodes.
    for (std::size_t e = 0; e < em_->nodes.size(); ++e)
        if (state_[e] == State::Changed || state_[e] == State::New)
            if (!generateBlock(e, genBlock_[e], genValues_[e]))
                return bail();

    out.clear();
    out.reserve(text_.size() + 1024);
    // Prefix: header, macro signature, comments before the first node.
    out += text_.substr(0, lines_.begin(firstUnit_));
    // Units: decorators stay in place, node slots take the nodes in write order.
    std::size_t next = 0;
    auto emitNode = [&](std::size_t e) {
        switch (state_[e]) {
        case State::Unchanged: {
            const Block& b = blocks_[static_cast<std::size_t>(origOf_[e])];
            out += text_.substr(lines_.begin(b.start), lines_.begin(b.end) - lines_.begin(b.start));
            break;
        }
        case State::Moved:
            out += movedBlock(e);
            break;
        default:
            out += genBlock_[e];
        }
    };
    for (const Unit& u : units_) {
        if (u.node < 0) {
            out += text_.substr(lines_.begin(u.start), lines_.begin(u.end) - lines_.begin(u.start));
        } else if (next < order_.size()) {
            emitNode(order_[next++]);
        }
    }
    while (next < order_.size())
        emitNode(order_[next++]);

    // After the body: CacheScene lines of deleted Images go, new Images get one.
    {
        std::size_t pos = lines_.begin(bodyEnd_);
        const std::size_t closeLineBegin = lines_.begin(toks_[closeTok_].line);
        const bool braceAlone = allBlank(text_.substr(closeLineBegin, toks_[closeTok_].begin - closeLineBegin));
        const std::size_t insertAt = braceAlone ? closeLineBegin : toks_[closeTok_].begin;
        std::set<int> deletedImages;
        for (std::size_t o = 0; o < om_->nodes.size(); ++o)
            if (editedOf_[o] < 0 && om_->nodes[o].module == "Image")
                deletedImages.insert(om_->nodes[o].instance);
        std::size_t i = closeTok_;
        while (i > 0 && toks_[i - 1].begin >= pos) --i;
        for (; i < closeTok_; ++i) {
            if (!toks_[i].isIdent("CacheScene"))
                continue;
            std::size_t k = i;
            while (k < closeTok_ && !toks_[k].is(";")) ++k;
            int inst = -1;
            for (std::size_t j = i; j < k && inst < 0; ++j) {
                std::string module;
                int in = 0, index = 0;
                bool input = false;
                if (toks_[j].kind == Tk::Ident && splitIoName(toks_[j].text, prefix_, module, in, input, index) &&
                    module == "Image")
                    inst = in;
            }
            if (inst >= 0 && deletedImages.count(inst)) {
                std::size_t b = toks_[i].begin, e = k < closeTok_ ? toks_[k].end : toks_[i].end;
                wholeLines(text_, b, e);
                out += text_.substr(pos, b - pos);
                pos = e;
            }
            i = k;
        }
        out += text_.substr(pos, insertAt - pos);
        if (!braceAlone)
            out += eol_;
        for (std::size_t e : order_)
            if (state_[e] == State::New && em_->nodes[e].module == "Image") {
                const std::string img = prefix_ + "Image_" + std::to_string(em_->nodes[e].instance);
                out += "CacheScene(" + img + "_in_1, " + img + "_out_1, " + img + "_out_2);" + eol_;
            }
        pos = insertAt;
        out += text_.substr(pos, valuesBegin_ - pos);
    }

    // Values section.
    {
        std::size_t pos = valuesBegin_;
        std::size_t insertAt = lines_.end(toks_[closeTok_].line);
        for (const Item& it : items_)
            if (it.node >= 0)
                insertAt = std::max(insertAt, it.end);
        std::set<int> placed;   // edited nodes whose group has been written
        std::string extra;      // groups with no place of their own, written at insertAt
        bool wrapperWritten = false;
        for (std::size_t e : order_) {
            const bool hasItems = origOf_[e] >= 0 &&
                                  std::any_of(items_.begin(), items_.end(), [&](const Item& it) { return it.node == origOf_[e]; });
            if (state_[e] == State::New || (state_[e] == State::Changed && !hasItems)) {
                if (state_[e] == State::New && em_->nodes[e].module == "Image" && wrapperNeeded_ && !wrapperWritten) {
                    extra += std::string(legacyImageMacroText());
                    wrapperWritten = true;
                }
                extra += valueGroup(e, genValues_[e]);
            } else if (!needOut_[e].empty() && !hasItems) {
                for (int k : needOut_[e]) {
                    const NetNode& n = em_->nodes[e];
                    std::string v = "NULL";
                    for (const NetOutput& o : n.outputs)
                        if (o.index == k && o.hasValue) v = o.value;
                    extra += ownOut(n, k) + " = " + v + ";" + eol_;
                }
            }
        }
        // Each item's last position per node, for needOut lines.
        std::map<int, std::size_t> lastItem;
        for (std::size_t i = 0; i < items_.size(); ++i)
            if (items_[i].node >= 0) lastItem[items_[i].node] = i;
        bool extraDone = false;
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const Item& it = items_[i];
            if (it.begin < pos)
                continue;
            if (!extraDone && insertAt <= it.begin) {
                out += text_.substr(pos, insertAt - pos);
                out += extra;
                pos = insertAt;
                extraDone = true;
            }
            out += text_.substr(pos, it.begin - pos);
            pos = it.end;
            if (it.isMacro) {
                if (wrapperUpgrade_ && it.macroName == "Image")
                    out += legacyImageMacroText();
                else
                    out += text_.substr(it.begin, it.end - it.begin);
                continue;
            }
            const int e = editedOf_[static_cast<std::size_t>(it.node)];
            if (e < 0)
                continue;   // deleted node
            const auto ue = static_cast<std::size_t>(e);
            if (state_[ue] == State::Changed) {
                if (placed.insert(e).second)
                    out += valueGroup(ue, genValues_[ue]);
                continue;
            }
            out += text_.substr(it.begin, it.end - it.begin);
            if (lastItem[it.node] == i && !needOut_[ue].empty()) {
                const NetNode& n = em_->nodes[ue];
                for (int k : needOut_[ue]) {
                    std::string v = "NULL";
                    for (const NetOutput& o : n.outputs)
                        if (o.index == k && o.hasValue) v = o.value;
                    out += ownOut(n, k) + " = " + v + ";" + eol_;
                }
            }
        }
        if (!extraDone) {
            if (insertAt < pos)
                insertAt = pos;
            out += text_.substr(pos, insertAt - pos);
            out += extra;
            pos = insertAt;
        }
        out += text_.substr(pos);
    }
    return true;
}

} // namespace

bool writeNodePositions(std::string_view original, const NetMacro& macro, std::string& out,
                        std::string* error)
{
    // Lines with their endings, so joining them restores the text exactly.
    std::vector<std::string> lines;
    for (std::size_t pos = 0; pos < original.size();) {
        std::size_t nl = original.find('\n', pos);
        std::size_t end = nl == std::string_view::npos ? original.size() : nl + 1;
        lines.emplace_back(original.substr(pos, end - pos));
        pos = end;
    }

    // `// node Module[inst]: x = 12, y = 34, ...`; groups: prefix, x, middle, y.
    static const std::regex nodeLine(
        R"(^(\s*//\s*node\s+(\w+)\[(\d+)\]:\s*x\s*=\s*)(-?\d+)(\s*,\s*y\s*=\s*)(-?\d+))");
    for (const NetNode& node : macro.nodes) {
        const std::string what = node.module + "[" + std::to_string(node.instance) + "]";
        if (node.line < 1 || static_cast<std::size_t>(node.line) > lines.size()) {
            fail(error, what + ": no node comment at line " + std::to_string(node.line));
            return false;
        }
        std::string& line = lines[static_cast<std::size_t>(node.line) - 1];
        std::smatch m;
        if (!std::regex_search(line, m, nodeLine) || m[2] != node.module ||
            std::stoi(m[3]) != node.instance) {
            fail(error, what + ": line " + std::to_string(node.line) +
                            " is not its node comment");
            return false;
        }
        const long x = std::lround(node.x), y = std::lround(node.y);
        line = m[1].str() + std::to_string(x) + m[5].str() + std::to_string(y) +
               m.suffix().str();
    }

    out.clear();
    out.reserve(original.size() + 64);
    for (const std::string& l : lines)
        out += l;
    return true;
}

bool writeNet(std::string_view originalText, const NetFile& original, const NetFile& edited,
              const ModuleLibrary* lib, std::string& out, std::string* error)
{
    try {
        Writer w(originalText, original, edited, lib);
        return w.run(out, error);
    } catch (const std::exception& e) {
        fail(error, std::string("internal error: ") + e.what());
    } catch (...) {
        fail(error, "internal error");
    }
    return false;
}

bool saveTextAtomically(const fs::path& path, std::string_view text, std::string* error)
{
    fs::path tmp = path;
    tmp += ".dxui2-save";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            fail(error, "cannot write " + tmp.string());
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.close();
        if (!f) {
            fail(error, "error writing " + tmp.string());
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    if (fs::exists(path, ec))
        fs::permissions(tmp, fs::status(path, ec).permissions(), ec);
    fs::rename(tmp, path, ec);
    if (ec) {
        fail(error, "cannot replace " + path.string() + ": " + ec.message());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace dxui
