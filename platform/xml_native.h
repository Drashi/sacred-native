#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sacred {
struct XmlTreeNode {
    enum Kind { Element, Text, Cdata, Comment } kind = Element;
    std::string name, value;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<std::unique_ptr<XmlTreeNode>> children;
    uint32_t line = 0;
};
struct XmlTreeDocument {
    std::string version = "1.0", encoding;
    int standalone = -1;
    std::vector<std::unique_ptr<XmlTreeNode>> children;
};
bool xml_parse_file(const std::string& path, XmlTreeDocument& document, std::string& error);
bool xml_write_file(const std::string& path, const XmlTreeDocument& document, bool indent, const std::string& encoding,
                    std::string& error);
}
