#pragma once
#include "../process/control.h"
#include "../../platform/xml_native.h"
#include <set>

namespace sacred {
class Startup;

// Guest-layout libxml2 shadow tree: the guest memory structures are the only tree; native
// libxml2 is used for parsing and serialization and never exposes a native pointer.
class Xml {
public:
    Xml(Control& control, const Loader& loader, Startup* files) : control_(control), loader_(loader), files_(files) {}
    ~Xml();
    void bind();

private:
    Control& control_;
    const Loader& loader_;
    Startup* files_;
    std::vector<uint32_t> chunks_;
    uint32_t bump_ = 0, limit_ = 0, indent_flag_ = 0, text_name_ = 0, comment_name_ = 0;
    std::map<uint32_t, uint32_t> sizes_;
    std::map<uint32_t, std::vector<uint32_t>> free_;
    std::set<uint32_t> documents_, nodes_;
    uint32_t allocate(uint32_t bytes);
    void release(uint32_t address);
    uint32_t make_string(const std::string& value);
    std::string read_string(uint32_t address) const;
    uint32_t field(uint32_t object, uint32_t offset) const { return control_.memory.load32(object + offset); }
    void set(uint32_t object, uint32_t offset, uint32_t value) { control_.memory.store32(object + offset, value); }
    uint32_t make_document(const std::string& version, uint32_t properties);
    uint32_t make_node(uint32_t type, uint32_t name, uint32_t content);
    uint32_t make_attribute(uint32_t node, const std::string& name, const std::string& value);
    uint32_t make_text(uint32_t document, const std::string& content, uint32_t type = 3);
    void append(uint32_t parent, uint32_t child);
    void unlink(uint32_t node);
    void adopt(uint32_t node, uint32_t document);
    void build(uint32_t document, uint32_t parent, const XmlTreeNode& source);
    void free_node(uint32_t node);
    void free_document(uint32_t document);
    uint32_t find_attribute(uint32_t node, const std::string& name) const;
    std::string attribute_value(uint32_t attribute) const;
    std::unique_ptr<XmlTreeNode> export_node(uint32_t node) const;
    uint32_t checked_node(uint32_t node, const char* what) const;
    uint32_t checked_document(uint32_t document) const;
    static std::string decode_entities(const std::string& content);
};
}
