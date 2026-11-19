#include "xml.h"
#include "../win32/startup.h"
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace sacred {
namespace {
// 32-bit libxml2 field offsets.
constexpr uint32_t type_at = 4, name_at = 8, children_at = 12, last_at = 16, parent_at = 20, next_at = 24, prev_at = 28,
                   doc_at = 32;
constexpr uint32_t node_size = 60, node_content = 40, node_properties = 44, node_line = 56;
constexpr uint32_t attribute_size = 48, attribute_type = 40;
constexpr uint32_t document_size = 96, document_compression = 36, document_standalone = 40, document_version = 56,
                   document_encoding = 60, document_url = 72, document_charset = 76, document_properties = 92;
constexpr uint32_t element = 1, attribute = 2, text = 3, cdata = 4, comment = 8, document = 9;
constexpr uint32_t chunk_bytes = 0x10000, inline_limit = 0x4000;
}

Xml::~Xml() {
    for (uint32_t chunk : chunks_) {
        control_.heap.free(chunk);
    }
}

uint32_t Xml::allocate(uint32_t bytes) {
    bytes = (std::max(bytes, 1u) + 15) & ~15u;
    if (bytes > inline_limit) {
        uint32_t block = control_.heap.allocate(bytes);
        chunks_.push_back(block);
        sizes_[block] = bytes;
        return block;
    }

    auto& recycled = free_[bytes];
    uint32_t address;
    if (!recycled.empty()) {
        address = recycled.back();
        recycled.pop_back();
    } else {
        if (bump_ + bytes > limit_) {
            bump_ = control_.heap.allocate(chunk_bytes);
            limit_ = bump_ + chunk_bytes;
            chunks_.push_back(bump_);
        }

        address = bump_;
        bump_ += bytes;
    }

    sizes_[address] = bytes;
    std::vector<uint8_t> zero(bytes);
    control_.memory.write(address, zero.data(), zero.size());
    return address;
}

void Xml::release(uint32_t address) {
    auto found = sizes_.find(address);
    if (found == sizes_.end()) {
        throw GuestFault("release of an unknown XML allocation");
    }

    if (found->second > inline_limit) {
        control_.heap.free(address);
        chunks_.erase(std::find(chunks_.begin(), chunks_.end(), address));
    } else {
        free_[found->second].push_back(address);
    }

    sizes_.erase(found);
}

uint32_t Xml::make_string(const std::string& value) {
    uint32_t address = allocate(uint32_t(value.size()) + 1);
    control_.memory.write(address, value.c_str(), value.size() + 1);
    return address;
}

std::string Xml::read_string(uint32_t address) const {
    std::string value;
    for (;;) {
        char byte;
        control_.memory.read(address + uint32_t(value.size()), &byte, 1);
        if (!byte) {
            return value;
        }

        value.push_back(byte);
        if (value.size() > (1u << 24)) {
            throw GuestFault("unterminated XML string");
        }
    }
}

uint32_t Xml::checked_document(uint32_t value) const {
    if (!documents_.count(value)) {
        throw GuestFault("unknown XML document pointer");
    }

    return value;
}

uint32_t Xml::checked_node(uint32_t value, const char* what) const {
    if (!nodes_.count(value)) {
        throw GuestFault(std::string("unknown XML ") + what + " pointer");
    }

    return value;
}

uint32_t Xml::make_document(const std::string& version, uint32_t properties) {
    uint32_t doc = allocate(document_size);
    set(doc, type_at, ::sacred::document);
    set(doc, doc_at, doc);
    set(doc, document_compression, uint32_t(-1));
    set(doc, document_standalone, uint32_t(-1));
    set(doc, document_version, make_string(version));
    set(doc, document_charset, 1);
    set(doc, document_properties, properties);
    documents_.insert(doc);
    return doc;
}

uint32_t Xml::make_node(uint32_t type, uint32_t name, uint32_t content) {
    uint32_t node = allocate(type == attribute ? attribute_size : node_size);
    set(node, type_at, type);
    set(node, name_at, name);
    if (type != attribute) {
        set(node, node_content, content);
    }

    nodes_.insert(node);
    return node;
}

uint32_t Xml::make_text(uint32_t doc, const std::string& content, uint32_t type) {
    uint32_t node = make_node(type, type == comment ? comment_name_ : text_name_, make_string(content));
    set(node, doc_at, doc);
    return node;
}

void Xml::append(uint32_t parent, uint32_t child) {
    set(child, parent_at, parent);
    set(child, doc_at,
        parent == 0 ? 0 : (field(parent, type_at) == ::sacred::document ? parent : field(parent, doc_at)));
    uint32_t last = field(parent, last_at);
    if (last) {
        set(last, next_at, child);
        set(child, prev_at, last);
    } else {
        set(parent, children_at, child);
    }

    set(parent, last_at, child);
}

void Xml::unlink(uint32_t node) {
    uint32_t parent = field(node, parent_at), previous = field(node, prev_at), next = field(node, next_at);
    if (!parent) {
        return;
    }

    if (previous) {
        set(previous, next_at, next);
    } else if (field(parent, children_at) == node) {
        set(parent, children_at, next);
    }

    if (next) {
        set(next, prev_at, previous);
    }

    if (field(parent, last_at) == node) {
        set(parent, last_at, previous);
    }

    set(node, parent_at, 0);
    set(node, next_at, 0);
    set(node, prev_at, 0);
}

void Xml::adopt(uint32_t node, uint32_t doc) {
    set(node, doc_at, doc);
    if (field(node, type_at) == element) {
        for (uint32_t property = field(node, node_properties); property; property = field(property, next_at)) {
            set(property, doc_at, doc);
            for (uint32_t child = field(property, children_at); child; child = field(child, next_at)) {
                set(child, doc_at, doc);
            }
        }
    }

    if (field(node, type_at) == element) {
        for (uint32_t child = field(node, children_at); child; child = field(child, next_at)) {
            adopt(child, doc);
        }
    }
}

uint32_t Xml::make_attribute(uint32_t node, const std::string& name, const std::string& value) {
    uint32_t doc = field(node, doc_at);
    uint32_t property = make_node(attribute, make_string(name), 0);
    set(property, doc_at, doc);
    set(property, parent_at, node);
    set(property, attribute_type, 0);
    uint32_t child = make_text(doc, value);
    set(child, parent_at, property);
    set(property, children_at, child);
    set(property, last_at, child);
    uint32_t last = field(node, node_properties);
    if (!last) {
        set(node, node_properties, property);
    } else {
        while (field(last, next_at)) {
            last = field(last, next_at);
        }

        set(last, next_at, property);
        set(property, prev_at, last);
    }

    return property;
}

uint32_t Xml::find_attribute(uint32_t node, const std::string& name) const {
    for (uint32_t property = field(node, node_properties); property; property = field(property, next_at)) {
        if (read_string(field(property, name_at)) == name) {
            return property;
        }
    }

    return 0;
}

std::string Xml::attribute_value(uint32_t property) const {
    std::string value;
    for (uint32_t child = field(property, children_at); child; child = field(child, next_at)) {
        if (field(child, type_at) == text) {
            value += read_string(field(child, node_content));
        } else {
            throw GuestFault("unqualified XML attribute child node");
        }
    }

    return value;
}

void Xml::build(uint32_t doc, uint32_t parent, const XmlTreeNode& source) {
    uint32_t node = 0;
    switch (source.kind) {
    case XmlTreeNode::Element:
        node = make_node(element, make_string(source.name), 0);
        set(node, doc_at, doc);
        append(parent, node);
        set(node, node_line, source.line > 0xffff ? 0xffff : source.line);
        for (const auto& pair : source.attributes) {
            make_attribute(node, pair.first, pair.second);
        }

        for (const auto& child : source.children) {
            build(doc, node, *child);
        }

        return;
    case XmlTreeNode::Text:
        node = make_text(doc, source.value, text);
        break;
    case XmlTreeNode::Cdata:
        node = make_text(doc, source.value, cdata);
        break;
    case XmlTreeNode::Comment:
        node = make_text(doc, source.value, comment);
        break;
    }

    if (!node) {
        throw GuestFault("unqualified XML tree node kind");
    }

    append(parent, node);
}

void Xml::free_node(uint32_t node) {
    const uint32_t type = field(node, type_at);
    if (type == element || type == attribute) {
        for (uint32_t child = field(node, children_at), next; child; child = next) {
            next = field(child, next_at);
            free_node(child);
        }

        if (type == element) {
            for (uint32_t property = field(node, node_properties), next; property; property = next) {
                next = field(property, next_at);
                free_node(property);
            }
        }
    }

    uint32_t name = field(node, name_at);
    if (name && name != text_name_ && name != comment_name_) {
        release(name);
    }

    if (type != attribute && type != element && field(node, node_content)) {
        release(field(node, node_content));
    }

    nodes_.erase(node);
    release(node);
}

void Xml::free_document(uint32_t doc) {
    for (uint32_t child = field(doc, children_at), next; child; child = next) {
        next = field(child, next_at);
        free_node(child);
    }

    for (uint32_t offset : {document_version, document_encoding, document_url}) {
        if (field(doc, offset)) {
            release(field(doc, offset));
        }
    }

    documents_.erase(doc);
    release(doc);
}

std::unique_ptr<XmlTreeNode> Xml::export_node(uint32_t node) const {
    auto result = std::make_unique<XmlTreeNode>();
    switch (field(node, type_at)) {
    case element:
        result->name = read_string(field(node, name_at));
        for (uint32_t property = field(node, node_properties); property; property = field(property, next_at)) {
            result->attributes.emplace_back(read_string(field(property, name_at)), attribute_value(property));
        }

        for (uint32_t child = field(node, children_at); child; child = field(child, next_at)) {
            result->children.push_back(export_node(child));
        }

        return result;
    case text:
        result->kind = XmlTreeNode::Text;
        break;
    case cdata:
        result->kind = XmlTreeNode::Cdata;
        break;
    case comment:
        result->kind = XmlTreeNode::Comment;
        break;
    default:
        throw GuestFault("unqualified XML node type for serialization");
    }

    result->value = read_string(field(node, node_content));
    return result;
}

std::string Xml::decode_entities(const std::string& content) {
    std::string result;
    for (size_t at = 0; at < content.size(); ++at) {
        if (content[at] != '&') {
            result.push_back(content[at]);
            continue;
        }

        size_t end = content.find(';', at);
        if (end == std::string::npos) {
            throw GuestFault("unterminated XML entity reference");
        }

        std::string name = content.substr(at + 1, end - at - 1);
        if (name == "lt") {
            result.push_back('<');
        } else if (name == "gt") {
            result.push_back('>');
        } else if (name == "amp") {
            result.push_back('&');
        } else if (name == "quot") {
            result.push_back('"');
        } else if (name == "apos") {
            result.push_back('\'');
        } else if (name.size() > 1 && name[0] == '#') {
            uint32_t code =
                uint32_t(std::stoul(name.substr(name[1] == 'x' ? 2 : 1), nullptr, name[1] == 'x' ? 16 : 10));
            if (code == 0 || code > 0x10ffff) {
                throw GuestFault("invalid XML character reference");
            }

            if (code < 0x80) {
                result.push_back(char(code));
            } else if (code < 0x800) {
                result.push_back(char(0xc0 | code >> 6));
                result.push_back(char(0x80 | (code & 0x3f)));
            } else if (code < 0x10000) {
                result.push_back(char(0xe0 | code >> 12));
                result.push_back(char(0x80 | (code >> 6 & 0x3f)));
                result.push_back(char(0x80 | (code & 0x3f)));
            } else {
                result.push_back(char(0xf0 | code >> 18));
                result.push_back(char(0x80 | (code >> 12 & 0x3f)));
                result.push_back(char(0x80 | (code >> 6 & 0x3f)));
                result.push_back(char(0x80 | (code & 0x3f)));
            }
        } else {
            throw GuestFault("unqualified XML entity reference: " + name);
        }

        at = end;
    }

    return result;
}

void Xml::bind() {
    text_name_ = make_string("text");
    comment_name_ = make_string("comment");
    indent_flag_ = allocate(4);
    set(indent_flag_, 0, 1);
    const Signature one{Convention::Cdecl, ReturnChannel::Eax, {{4}}, ""};
    auto bind = [&](const char* symbol, Signature signature, HostFunction function) {
        control_.host(loader_.resolve("libxml2.dll", symbol), std::move(signature), std::move(function),
                      std::string("libxml2.dll!") + symbol);
    };
    bind("__xmlIndentTreeOutput", {Convention::Cdecl, ReturnChannel::Eax, {}, ""},
         [this](Control&, const std::vector<uint64_t>&) { return Result{indent_flag_, {}}; });
    bind("xmlNewDoc", one, [this](Control&, const std::vector<uint64_t>& a) {
        return Result{make_document(a[0] ? read_string(uint32_t(a[0])) : "1.0", 8), {}};
    });
    bind("xmlFreeDoc", {Convention::Cdecl, ReturnChannel::Void, {{4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (a[0]) {
                 free_document(checked_document(uint32_t(a[0])));
             }

             return Result{};
         });
    bind("xmlNewNode", {Convention::Cdecl, ReturnChannel::Eax, {{4}, {4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (a[0] || !a[1]) {
                 throw GuestFault("unqualified xmlNewNode namespace or name");
             }

             return Result{make_node(element, make_string(read_string(uint32_t(a[1]))), 0), {}};
         });
    bind("xmlDocGetRootElement", one, [this](Control&, const std::vector<uint64_t>& a) {
        if (!a[0]) {
            return Result{};
        }

        uint32_t doc = checked_document(uint32_t(a[0]));
        for (uint32_t child = field(doc, children_at); child; child = field(child, next_at)) {
            if (field(child, type_at) == element) {
                return Result{child, {}};
            }
        }

        return Result{};
    });
    bind("xmlDocSetRootElement", {Convention::Cdecl, ReturnChannel::Eax, {{4}, {4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (!a[0] || !a[1]) {
                 return Result{};
             }

             uint32_t doc = checked_document(uint32_t(a[0]));
             uint32_t root = checked_node(uint32_t(a[1]), "node");
             if (field(root, type_at) != element) {
                 throw GuestFault("xmlDocSetRootElement with a non-element node");
             }

             unlink(root);
             uint32_t old = 0;
             for (uint32_t child = field(doc, children_at); child; child = field(child, next_at)) {
                 if (field(child, type_at) == element) {
                     old = child;
                     break;
                 }
             }

             adopt(root, doc);
             if (!old) {
                 append(doc, root);
                 return Result{};
             }

             uint32_t previous = field(old, prev_at), next = field(old, next_at);
             set(root, parent_at, doc);
             set(root, prev_at, previous);
             set(root, next_at, next);
             if (previous) {
                 set(previous, next_at, root);
             } else {
                 set(doc, children_at, root);
             }

             if (next) {
                 set(next, prev_at, root);
             } else {
                 set(doc, last_at, root);
             }

             set(old, parent_at, 0);
             set(old, prev_at, 0);
             set(old, next_at, 0);
             return Result{old, {}};
         });
    bind("xmlNewChild", {Convention::Cdecl, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             uint32_t parent = uint32_t(a[0]);
             if (!parent) {
                 return Result{};
             }

             if (!documents_.count(parent)) {
                 checked_node(parent, "parent");
             }

             if (a[1] || !a[2]) {
                 throw GuestFault("unqualified xmlNewChild namespace or name");
             }

             uint32_t node = make_node(element, make_string(read_string(uint32_t(a[2]))), 0);
             append(parent, node);
             if (a[3]) {
                 uint32_t content = make_text(field(node, doc_at), decode_entities(read_string(uint32_t(a[3]))));
                 append(node, content);
             }

             return Result{node, {}};
         });
    bind("xmlSetProp", {Convention::Cdecl, ReturnChannel::Eax, {{4}, {4}, {4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (!a[0]) {
                 return Result{};
             }

             uint32_t node = checked_node(uint32_t(a[0]), "node");
             if (field(node, type_at) != element || !a[1]) {
                 return Result{};
             }

             std::string name = read_string(uint32_t(a[1])), value = a[2] ? read_string(uint32_t(a[2])) : std::string();
             if (uint32_t property = find_attribute(node, name)) {
                 for (uint32_t child = field(property, children_at), next; child; child = next) {
                     next = field(child, next_at);
                     free_node(child);
                 }

                 uint32_t child = make_text(field(property, doc_at), value);
                 set(child, parent_at, property);
                 set(property, children_at, child);
                 set(property, last_at, child);
                 return Result{property, {}};
             }

             return Result{make_attribute(node, name, value), {}};
         });
    bind("xmlGetProp", {Convention::Cdecl, ReturnChannel::Eax, {{4}, {4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (!a[0]) {
                 return Result{};
             }

             uint32_t node = checked_node(uint32_t(a[0]), "node");
             if (field(node, type_at) != element || !a[1]) {
                 return Result{};
             }

             uint32_t property = find_attribute(node, read_string(uint32_t(a[1])));
             return Result{property ? make_string(attribute_value(property)) : 0, {}};
         });
    bind("xmlStrcmp", {Convention::Cdecl, ReturnChannel::Eax, {{4}, {4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (a[0] == a[1]) {
                 return Result{};
             }

             if (!a[0]) {
                 return Result{uint32_t(-1), {}};
             }

             if (!a[1]) {
                 return Result{1, {}};
             }

             std::string left = read_string(uint32_t(a[0])), right = read_string(uint32_t(a[1]));
             size_t at = 0;
             while (at < left.size() && at < right.size() && left[at] == right[at]) {
                 ++at;
             }

             int difference =
                 int(uint8_t(at < left.size() ? left[at] : 0)) - int(uint8_t(at < right.size() ? right[at] : 0));
             return Result{uint32_t(difference), {}};
         });
    bind("xmlParseFile", one, [this](Control&, const std::vector<uint64_t>& a) {
        if (!files_) {
            throw GuestFault("xmlParseFile requires the guest filesystem");
        }

        std::string name = read_string(uint32_t(a[0]));
        auto path = files_->read_path(name);
        std::error_code status;
        if (!std::filesystem::is_regular_file(path, status)) {
            return Result{};
        }

        XmlTreeDocument parsed;
        std::string error;
        if (!xml_parse_file(path.string(), parsed, error)) {
            return Result{};
        }

        uint32_t doc = make_document(parsed.version, 1);
        set(doc, document_compression, 0);
        set(doc, document_standalone, uint32_t(parsed.standalone));
        set(doc, document_url, make_string(name));
        if (!parsed.encoding.empty()) {
            set(doc, document_encoding, make_string(parsed.encoding));
        }

        for (const auto& child : parsed.children) {
            build(doc, doc, *child);
        }

        return Result{doc, {}};
    });
    bind("xmlSaveFormatFileEnc", {Convention::Cdecl, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (!files_) {
                 throw GuestFault("xmlSaveFormatFileEnc requires the guest filesystem");
             }

             if (!a[1] || !a[0]) {
                 return Result{uint32_t(-1), {}};
             }

             uint32_t doc = checked_document(uint32_t(a[1]));
             auto path = files_->write_path(read_string(uint32_t(a[0])));
             std::filesystem::create_directories(path.parent_path());
             XmlTreeDocument tree;
             tree.version = field(doc, document_version) ? read_string(field(doc, document_version)) : "1.0";
             for (uint32_t child = field(doc, children_at); child; child = field(child, next_at)) {
                 tree.children.push_back(export_node(child));
             }

             std::string encoding = a[2] ? read_string(uint32_t(a[2])) : std::string(), error;
             const bool indent = int32_t(a[3]) == 1 && control_.memory.load32(indent_flag_) != 0;
             if (!xml_write_file(path.string(), tree, indent, encoding, error)) {
                 return Result{uint32_t(-1), {}};
             }

             return Result{uint32_t(std::filesystem::file_size(path)), {}};
         });
}
}
