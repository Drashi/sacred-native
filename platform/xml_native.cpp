#include "xml_native.h"
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <fstream>
#include <sstream>

// Our own XML reader and writer for the game's libxml2 calls (xmlParseFile, xmlSaveFormatFileEnc), replacing the
// libxml2 library. The game reads and writes small configuration files (guiconfig.xml): elements, attributes, text,
// CDATA and comments, in UTF-8 or ISO-8859-1. The reader gives the tree libxml2's text reader gives (blank text kept,
// adjacent text and references merged, attribute values normalized); the writer produces the bytes of libxml2's
// xmlTextWriter (declaration, indentation, escapes, re-encoding with character references).
namespace sacred {
namespace {
bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_name_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u) || c == '_' || c == ':' || c == '-' || c == '.';
}

std::string lower(std::string text) {
    for (char& c : text) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }

    return text;
}

bool latin1(const std::string& encoding) {
    const std::string name = lower(encoding);
    return name == "iso-8859-1" || name == "iso8859-1" || name == "latin1" || name == "latin-1" || name == "l1" ||
           name == "iso_8859-1" || name == "iso-latin-1";
}

bool utf8(const std::string& encoding) {
    const std::string name = lower(encoding);
    return name.empty() || name == "utf-8" || name == "utf8" || name == "us-ascii" || name == "ascii";
}

void append_utf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += char(code);
    } else if (code < 0x800) {
        out += char(0xc0 | code >> 6);
        out += char(0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
        out += char(0xe0 | code >> 12);
        out += char(0x80 | (code >> 6 & 0x3f));
        out += char(0x80 | (code & 0x3f));
    } else {
        out += char(0xf0 | code >> 18);
        out += char(0x80 | (code >> 12 & 0x3f));
        out += char(0x80 | (code >> 6 & 0x3f));
        out += char(0x80 | (code & 0x3f));
    }
}

// Next code point of UTF-8 text; malformed bytes are taken one by one as Latin-1.
uint32_t next_utf8(const std::string& text, size_t& at) {
    const auto byte = [&](size_t i) {
        return static_cast<unsigned char>(text[i]);
    };
    const unsigned char first = byte(at);
    int extra = first >= 0xf0 && first < 0xf8 ? 3 : first >= 0xe0 ? 2 : first >= 0xc0 ? 1 : 0;
    if (first < 0x80 || !extra) {
        ++at;
        return first;
    }

    uint32_t code = first & (0x3f >> extra);
    for (int i = 1; i <= extra; ++i) {
        if (at + i >= text.size() || (byte(at + i) & 0xc0) != 0x80) {
            ++at;
            return first;
        }

        code = code << 6 | (byte(at + i) & 0x3f);
    }

    at += size_t(extra) + 1;
    return code;
}

class Parser {
public:
    Parser(std::string text, XmlTreeDocument& document) : text_(std::move(text)), document_(document) {}
    bool run(std::string& error) {
        try {
            parse();
            return true;
        } catch (const std::runtime_error& failure) {
            error = failure.what();
            return false;
        }
    }

private:
    std::string text_;
    XmlTreeDocument& document_;
    size_t at_ = 0;
    uint32_t line_ = 1;

    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error("XML is not well-formed: " + what + " at line " + std::to_string(line_));
    }

    bool starts(const char* literal) const {
        return text_.compare(at_, std::char_traits<char>::length(literal), literal) == 0;
    }

    char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }
    void advance(size_t count = 1) {
        for (size_t i = 0; i < count && at_ < text_.size(); ++i, ++at_) {
            if (text_[at_] == '\n') {
                ++line_;
            }
        }
    }

    void expect(const char* literal) {
        if (!starts(literal)) {
            fail(std::string("expected '") + literal + "'");
        }

        advance(std::char_traits<char>::length(literal));
    }

    void skip_space() {
        while (at_ < text_.size() && is_space(text_[at_])) {
            advance();
        }
    }

    std::string name() {
        const size_t begin = at_;
        while (at_ < text_.size() && is_name_char(text_[at_])) {
            advance();
        }

        if (begin == at_) {
            fail("expected a name");
        }

        return text_.substr(begin, at_ - begin);
    }

    // One reference after '&': predefined entities and character references.
    void reference(std::string& out) {
        advance();
        if (peek() == '#') {
            advance();
            const bool hex = peek() == 'x';
            if (hex) {
                advance();
            }

            uint32_t code = 0;
            size_t digits = 0;
            while (std::isxdigit(static_cast<unsigned char>(peek())) &&
                   (hex || std::isdigit(static_cast<unsigned char>(peek())))) {
                code = code * (hex ? 16 : 10) +
                       uint32_t(std::isdigit(static_cast<unsigned char>(peek()))
                                    ? peek() - '0'
                                    : std::tolower(static_cast<unsigned char>(peek())) - 'a' + 10);
                if (code > 0x10ffff) {
                    fail("character reference out of range");
                }

                advance();
                ++digits;
            }

            if (!digits || peek() != ';' || !code) {
                fail("malformed character reference");
            }

            advance();
            append_utf8(out, code);
            return;
        }

        const std::string entity = name();
        if (peek() != ';') {
            fail("malformed entity reference");
        }

        advance();
        if (entity == "lt") {
            out += '<';
        } else if (entity == "gt") {
            out += '>';
        } else if (entity == "amp") {
            out += '&';
        } else if (entity == "quot") {
            out += '"';
        } else if (entity == "apos") {
            out += '\'';
        } else {
            fail("undeclared entity '" + entity + "'");
        }
    }

    std::string quoted_value(bool attribute) {
        const char quote = peek();
        if (quote != '"' && quote != '\'') {
            fail("expected a quoted value");
        }

        advance();
        std::string value;
        while (peek() != quote) {
            if (at_ >= text_.size()) {
                fail("unterminated value");
            }

            if (attribute && peek() == '<') {
                fail("'<' in an attribute value");
            }

            if (attribute && peek() == '&') {
                reference(value);
                continue;
            }

            // Attribute value normalization: literal white space becomes a space (line ends are already '\n').
            value += attribute && is_space(peek()) ? ' ' : peek();
            advance();
        }

        advance();
        return value;
    }

    void declaration() {
        // <?xml version="1.0" encoding="..." standalone="..."?>
        expect("<?xml");
        skip_space();
        std::string version = "1.0", encoding;
        int standalone = -2; // declaration without standalone, as libxml2 reports it
        while (!starts("?>")) {
            if (at_ >= text_.size()) {
                fail("unterminated XML declaration");
            }

            const std::string key = name();
            skip_space();
            expect("=");
            skip_space();
            const std::string value = quoted_value(false);
            if (key == "version") {
                version = value;
            } else if (key == "encoding") {
                encoding = value;
            } else if (key == "standalone") {
                standalone = value == "yes" ? 1 : 0;
            } else {
                fail("unknown XML declaration attribute '" + key + "'");
            }

            skip_space();
        }

        advance(2);
        document_.version = version;
        document_.encoding = encoding;
        document_.standalone = standalone;
        if (latin1(encoding)) {
            // Convert the rest of the document to UTF-8, as libxml2 does with its input.
            std::string converted = text_.substr(0, at_);
            for (size_t i = at_; i < text_.size(); ++i) {
                append_utf8(converted, static_cast<unsigned char>(text_[i]));
            }

            text_ = std::move(converted);
        } else if (!utf8(encoding)) {
            fail("unsupported encoding '" + encoding + "'");
        }
    }

    std::unique_ptr<XmlTreeNode> node(XmlTreeNode::Kind kind, uint32_t line) {
        auto made = std::make_unique<XmlTreeNode>();
        made->kind = kind;
        made->line = line;
        return made;
    }

    std::unique_ptr<XmlTreeNode> comment() {
        const uint32_t line = line_;
        advance(4);
        const size_t end = text_.find("-->", at_);
        if (end == std::string::npos) {
            fail("unterminated comment");
        }

        auto made = node(XmlTreeNode::Comment, line);
        made->value = text_.substr(at_, end - at_);
        advance(end + 3 - at_);
        return made;
    }

    void misc(std::vector<std::unique_ptr<XmlTreeNode>>& into) {
        for (;;) {
            skip_space();
            if (starts("<!--")) {
                into.push_back(comment());
            } else if (starts("<?")) {
                fail("processing instructions are not supported");
            } else if (starts("<!DOCTYPE")) {
                fail("document type declarations are not supported");
            } else {
                return;
            }
        }
    }

    std::unique_ptr<XmlTreeNode> element() {
        const uint32_t line = line_;
        expect("<");
        auto made = node(XmlTreeNode::Element, line);
        made->name = name();
        for (;;) {
            const bool spaced = is_space(peek());
            skip_space();
            if (starts("/>")) {
                advance(2);
                return made;
            }

            if (peek() == '>') {
                advance();
                break;
            }

            if (!spaced) {
                fail("expected white space between attributes");
            }

            std::string key = name();
            skip_space();
            expect("=");
            skip_space();
            for (const auto& attribute : made->attributes) {
                if (attribute.first == key) {
                    fail("attribute '" + key + "' given twice");
                }
            }

            made->attributes.emplace_back(std::move(key), quoted_value(true));
        }

        content(*made);
        expect("</");
        if (name() != made->name) {
            fail("end tag does not match '" + made->name + "'");
        }

        skip_space();
        expect(">");
        return made;
    }

    void content(XmlTreeNode& parent) {
        XmlTreeNode* text = nullptr; // adjacent character data and references form one text node
        for (;;) {
            if (at_ >= text_.size()) {
                fail("unterminated element '" + parent.name + "'");
            }

            if (starts("</")) {
                return;
            }

            if (starts("<![CDATA[")) {
                const uint32_t line = line_;
                advance(9);
                const size_t end = text_.find("]]>", at_);
                if (end == std::string::npos) {
                    fail("unterminated CDATA section");
                }

                auto made = node(XmlTreeNode::Cdata, line);
                made->value = text_.substr(at_, end - at_);
                advance(end + 3 - at_);
                parent.children.push_back(std::move(made));
                text = nullptr;
            } else if (starts("<!--")) {
                parent.children.push_back(comment());
                text = nullptr;
            } else if (starts("<?") || starts("<!")) {
                fail("unsupported markup in content");
            } else if (peek() == '<') {
                parent.children.push_back(element());
                text = nullptr;
            } else {
                if (!text) {
                    parent.children.push_back(node(XmlTreeNode::Text, line_));
                    text = parent.children.back().get();
                }

                if (peek() == '&') {
                    reference(text->value);
                    continue;
                }

                if (starts("]]>")) {
                    fail("']]>' in content");
                }

                text->value += peek();
                advance();
            }
        }
    }

    void parse() {
        // Line ends: CR LF and lone CR become LF before anything else (XML 1.0, 2.11).
        std::string normalized;
        normalized.reserve(text_.size());
        for (size_t i = 0; i < text_.size(); ++i) {
            if (text_[i] == '\r') {
                normalized += '\n';
                if (i + 1 < text_.size() && text_[i + 1] == '\n') {
                    ++i;
                }
            } else {
                normalized += text_[i];
            }
        }

        text_ = std::move(normalized);
        if (text_.compare(0, 3, "\xef\xbb\xbf") == 0) {
            at_ = 3;
        }

        document_.version = "1.0";
        document_.encoding.clear();
        document_.standalone = -1;
        if (starts("<?xml") && at_ + 5 < text_.size() && is_space(text_[at_ + 5])) {
            declaration();
        }

        misc(document_.children);
        if (peek() != '<') {
            fail("no root element");
        }

        document_.children.push_back(element());
        misc(document_.children);
        if (at_ != text_.size()) {
            fail("content after the root element");
        }
    }
};

class Writer {
public:
    Writer(bool indent, std::string encoding) : indent_(indent), encoding_(std::move(encoding)) {}
    std::string document(const XmlTreeDocument& tree) {
        raw("<?xml version=\"" + tree.version + "\"");
        if (!encoding_.empty()) {
            raw(" encoding=\"" + encoding_ + "\"");
        }

        raw("?>\n");
        for (const auto& child : tree.children) {
            node(*child);
        }

        close_tag();
        if (!indent_) {
            raw("\n");
        }

        return out_;
    }

private:
    bool indent_;
    std::string encoding_;
    std::string out_;
    size_t depth_ = 0;      // open elements
    bool open_tag_ = false; // "<name ..." written, '>' not yet
    bool do_indent_ = true; // the next end tag goes on its own indented line

    // Text that passes the output encoder: UTF-8 stays, ISO-8859-1 takes code points up to 0xff and turns any
    // other into a decimal character reference (libxml2's encoder fallback).
    void encoded(const std::string& text) {
        if (!latin1(encoding_)) {
            out_ += text;
            return;
        }

        for (size_t at = 0; at < text.size();) {
            const uint32_t code = next_utf8(text, at);
            if (code <= 0xff) {
                out_ += char(code);
            } else {
                out_ += "&#" + std::to_string(code) + ";";
            }
        }
    }

    void raw(const std::string& text) { encoded(text); }
    void indentation(size_t levels) {
        for (size_t i = 0; i < levels; ++i) {
            out_ += "  ";
        }
    }

    void close_tag() {
        if (!open_tag_) {
            return;
        }

        out_ += ">";
        open_tag_ = false;
    }

    static std::string content_escaped(const std::string& text) {
        std::string out;
        for (char c : text) {
            if (c == '<') {
                out += "&lt;";
            } else if (c == '>') {
                out += "&gt;";
            } else if (c == '&') {
                out += "&amp;";
            } else if (c == '"') {
                out += "&quot;";
            } else if (c == '\r') {
                out += "&#13;";
            } else {
                out += c;
            }
        }

        return out;
    }

    // Attribute values: without an output encoding libxml2 turns every non-ASCII character into a hexadecimal
    // character reference; with one, the encoder sees them as for content.
    std::string attribute_escaped(const std::string& text) const {
        std::string out;
        for (size_t at = 0; at < text.size();) {
            const char c = text[at];
            if (static_cast<unsigned char>(c) >= 0x80 && !encoding_.empty()) {
                out += c;
                ++at;
                continue;
            }

            if (static_cast<unsigned char>(c) >= 0x80) {
                char buffer[16];
                std::snprintf(buffer, sizeof buffer, "&#x%X;", next_utf8(text, at));
                out += buffer;
                continue;
            }

            ++at;
            if (c == '<') {
                out += "&lt;";
            } else if (c == '>') {
                out += "&gt;";
            } else if (c == '&') {
                out += "&amp;";
            } else if (c == '"') {
                out += "&quot;";
            } else if (c == '\n') {
                out += "&#10;";
            } else if (c == '\r') {
                out += "&#13;";
            } else if (c == '\t') {
                out += "&#9;";
            } else {
                out += c;
            }
        }

        return out;
    }

    void node(const XmlTreeNode& item) {
        switch (item.kind) {
        case XmlTreeNode::Element:
            if (open_tag_) {
                close_tag();
                if (indent_) {
                    out_ += "\n";
                }
            }

            if (indent_) {
                indentation(depth_);
            }

            out_ += "<";
            encoded(item.name);
            for (const auto& attribute : item.attributes) {
                out_ += " ";
                encoded(attribute.first);
                out_ += "=\"";
                encoded(attribute_escaped(attribute.second));
                out_ += "\"";
            }

            open_tag_ = true;
            ++depth_;
            for (const auto& child : item.children) {
                node(*child);
            }

            --depth_;
            if (open_tag_) {
                out_ += "/>";
                open_tag_ = false;
                if (indent_) {
                    do_indent_ = true;
                }
            } else {
                if (indent_ && do_indent_) {
                    indentation(depth_);
                }

                do_indent_ = true;
                out_ += "</";
                encoded(item.name);
                out_ += ">";
            }

            if (indent_) {
                out_ += "\n";
            }

            break;
        case XmlTreeNode::Text:
            close_tag();
            if (indent_) {
                do_indent_ = false;
            }

            encoded(content_escaped(item.value));
            break;
        case XmlTreeNode::Cdata:
            close_tag();
            if (indent_) {
                do_indent_ = false;
            }

            out_ += "<![CDATA[";
            encoded(item.value);
            out_ += "]]>";
            break;
        case XmlTreeNode::Comment:
            if (open_tag_) {
                close_tag();
                if (indent_) {
                    out_ += "\n";
                }
            }

            if (indent_) {
                indentation(depth_);
            }

            out_ += "<!--";
            encoded(item.value);
            out_ += "-->";
            if (indent_) {
                out_ += "\n";
                do_indent_ = false;
            }

            break;
        }
    }
};
}

bool xml_parse_file(const std::string& path, XmlTreeDocument& document, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open XML file";
        return false;
    }

    std::ostringstream bytes;
    bytes << file.rdbuf();
    return Parser(bytes.str(), document).run(error);
}

bool xml_write_file(const std::string& path, const XmlTreeDocument& document, bool indent, const std::string& encoding,
                    std::string& error) {
    if (!utf8(encoding) && !latin1(encoding)) {
        error = "unsupported XML output encoding " + encoding;
        return false;
    }

    const std::string text = Writer(indent, encoding).document(document);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot create XML file";
        return false;
    }

    file.write(text.data(), std::streamsize(text.size()));
    if (!file) {
        error = "cannot write XML file";
        return false;
    }

    return true;
}
}
