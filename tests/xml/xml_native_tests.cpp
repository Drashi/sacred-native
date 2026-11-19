// Our XML reader and writer (platform/xml_native.cpp), which replaced libxml2. The expected bytes were written by
// libxml2's xmlTextWriter for the same tree, and the parse results are what libxml2's text reader gives.
#include "platform/xml_native.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

using namespace sacred;
static unsigned checks = 0;
static void require(bool condition, const std::string& what) {
    ++checks;
    if (!condition) {
        throw std::runtime_error("FAIL: " + what);
    }
}

static std::unique_ptr<XmlTreeNode> element(const char* name) {
    auto made = std::make_unique<XmlTreeNode>();
    made->name = name;
    return made;
}

static std::unique_ptr<XmlTreeNode> leaf(XmlTreeNode::Kind kind, const char* value) {
    auto made = std::make_unique<XmlTreeNode>();
    made->kind = kind;
    made->value = value;
    return made;
}

static std::string dump(const XmlTreeNode& node) {
    std::string text = std::to_string(node.kind) + "[" + node.name + "|" + node.value;
    for (const auto& attribute : node.attributes) {
        text += " " + attribute.first + "=" + attribute.second;
    }

    for (const auto& child : node.children) {
        text += dump(*child);
    }

    return text + "]";
}

static std::string dump(const XmlTreeDocument& document) {
    std::string text = document.version + "|" + document.encoding + "|" + std::to_string(document.standalone);
    for (const auto& child : document.children) {
        text += dump(*child);
    }

    return text;
}

static std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << file.rdbuf();
    return bytes.str();
}

static void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
}

static const struct {
    int indent;
    const char* encoding;
    const char* bytes;
} written[] = {
    {0, "",
     "<?xml version=\"1.0\"?>\n"
     "<!-- gui --><GuiConfig version=\"2\"><Panel name=\"inv&#xE9;&#x20AC; &quot;&lt;&amp;&gt;&quot;&#10;&#9;\">a &lt; "
     "b &amp; &quot;c&quot; \xc3"
     "\xa9"
     "\xe2"
     "\x82"
     "\xac"
     "&#13;</Panel><Empty/><Box><![CDATA[<raw>]]><Inner><!--c--></Inner></Box></GuiConfig>\n"
     ""},
    {0, "ISO-8859-1",
     "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n"
     "<!-- gui --><GuiConfig version=\"2\"><Panel name=\"inv\xe9"
     "&#8364; &quot;&lt;&amp;&gt;&quot;&#10;&#9;\">a &lt; b &amp; &quot;c&quot; \xe9"
     "&#8364;&#13;</Panel><Empty/><Box><![CDATA[<raw>]]><Inner><!--c--></Inner></Box></GuiConfig>\n"
     ""},
    {0, "UTF-8",
     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
     "<!-- gui --><GuiConfig version=\"2\"><Panel name=\"inv\xc3"
     "\xa9"
     "\xe2"
     "\x82"
     "\xac"
     " &quot;&lt;&amp;&gt;&quot;&#10;&#9;\">a &lt; b &amp; &quot;c&quot; \xc3"
     "\xa9"
     "\xe2"
     "\x82"
     "\xac"
     "&#13;</Panel><Empty/><Box><![CDATA[<raw>]]><Inner><!--c--></Inner></Box></GuiConfig>\n"
     ""},
    {1, "",
     "<?xml version=\"1.0\"?>\n"
     "<!-- gui -->\n"
     "<GuiConfig version=\"2\">\n"
     "  <Panel name=\"inv&#xE9;&#x20AC; &quot;&lt;&amp;&gt;&quot;&#10;&#9;\">a &lt; b &amp; &quot;c&quot; \xc3"
     "\xa9"
     "\xe2"
     "\x82"
     "\xac"
     "&#13;</Panel>\n"
     "  <Empty/>\n"
     "  <Box><![CDATA[<raw>]]>    <Inner>\n"
     "      <!--c-->\n"
     "</Inner>\n"
     "  </Box>\n"
     "</GuiConfig>\n"
     ""},
    {1, "ISO-8859-1",
     "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n"
     "<!-- gui -->\n"
     "<GuiConfig version=\"2\">\n"
     "  <Panel name=\"inv\xe9"
     "&#8364; &quot;&lt;&amp;&gt;&quot;&#10;&#9;\">a &lt; b &amp; &quot;c&quot; \xe9"
     "&#8364;&#13;</Panel>\n"
     "  <Empty/>\n"
     "  <Box><![CDATA[<raw>]]>    <Inner>\n"
     "      <!--c-->\n"
     "</Inner>\n"
     "  </Box>\n"
     "</GuiConfig>\n"
     ""},
    {1, "UTF-8",
     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
     "<!-- gui -->\n"
     "<GuiConfig version=\"2\">\n"
     "  <Panel name=\"inv\xc3"
     "\xa9"
     "\xe2"
     "\x82"
     "\xac"
     " &quot;&lt;&amp;&gt;&quot;&#10;&#9;\">a &lt; b &amp; &quot;c&quot; \xc3"
     "\xa9"
     "\xe2"
     "\x82"
     "\xac"
     "&#13;</Panel>\n"
     "  <Empty/>\n"
     "  <Box><![CDATA[<raw>]]>    <Inner>\n"
     "      <!--c-->\n"
     "</Inner>\n"
     "  </Box>\n"
     "</GuiConfig>\n"
     ""},
};

int main() {
    try {
        const auto directory =
            std::filesystem::temp_directory_path() / ("xml_native_tests_" + std::to_string(::getpid()));
        std::filesystem::create_directories(directory);
        const auto path = directory / "doc.xml";
        XmlTreeDocument tree;
        tree.children.push_back(leaf(XmlTreeNode::Comment, " gui "));
        auto root = element("GuiConfig");
        root->attributes.emplace_back("version", "2");
        auto panel = element("Panel");
        panel->attributes.emplace_back("name", "inv\xc3\xa9\xe2\x82\xac \"<&>\"\n\t");
        panel->children.push_back(leaf(XmlTreeNode::Text, "a < b & \"c\" \xc3\xa9\xe2\x82\xac\r"));
        root->children.push_back(std::move(panel));
        root->children.push_back(element("Empty"));
        auto box = element("Box");
        box->children.push_back(leaf(XmlTreeNode::Cdata, "<raw>"));
        auto inner = element("Inner");
        inner->children.push_back(leaf(XmlTreeNode::Comment, "c"));
        box->children.push_back(std::move(inner));
        root->children.push_back(std::move(box));
        tree.children.push_back(std::move(root));
        std::string error;
        for (const auto& expected : written) {
            const std::string label =
                std::string("write indent=") + std::to_string(expected.indent) + " encoding=" + expected.encoding;
            require(xml_write_file(path.string(), tree, expected.indent, expected.encoding, error),
                    label + ": " + error);
            require(read(path) == expected.bytes, label + " bytes");
            // What we write reads back as the same tree (modulo the blank text the indentation adds).
            XmlTreeDocument back;
            require(xml_parse_file(path.string(), back, error), label + " reads back: " + error);
            const auto children = [](const XmlTreeDocument& document) {
                std::string text;
                for (const auto& child : document.children) {
                    text += dump(*child);
                }

                return text;
            };
            if (!expected.indent) {
                require(children(back) == children(tree), label + " reads back as " + dump(back));
            }
        }

        // Reading: ISO-8859-1 input, CR LF, references, attribute value normalization, CDATA and comments.
        write(path, "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\r\n<a x=\"1\t2\n3\" y='&lt;&#65;&#x42;'>caf\xe9 "
                    "&amp; &#233;\r\n"
                    "<b/><!-- c --><![CDATA[<raw>]]>tail</a>\r\n");
        XmlTreeDocument latin;
        require(xml_parse_file(path.string(), latin, error), "latin1 parses: " + error);
        require(dump(latin) ==
                    "1.0|ISO-8859-1|-20[a| x=1 2 3 y=<AB1[|caf\xc3\xa9 & \xc3\xa9\n]0[b|]3[| c ]2[|<raw>]1[|tail]]",
                "latin1 tree " + dump(latin));
        write(path, "\xef\xbb\xbf<root><x a=\"v\"/>  <y>  </y></root>");
        XmlTreeDocument bom;
        require(xml_parse_file(path.string(), bom, error), "BOM parses");
        require(dump(bom) == "1.0||-10[root|0[x| a=v]1[|  ]0[y|1[|  ]]]", "BOM tree " + dump(bom));
        write(path, "<?xml version=\"1.0\" standalone=\"yes\"?><r/>");
        XmlTreeDocument standalone;
        require(xml_parse_file(path.string(), standalone, error) && standalone.standalone == 1, "standalone");
        for (const char* bad : {"<r><a></b></r>", "<r>&foo;</r>", "<r a=\"1\" a=\"2\"/>", "<r>x</r>junk",
                                "<r a=\"1\"b=\"2\"/>", "<r>", "", "<?xml version=\"1.0\" encoding=\"KOI8-R\"?><r/>"}) {
            write(path, bad);
            XmlTreeDocument rejected;
            require(!xml_parse_file(path.string(), rejected, error), std::string("rejects ") + bad);
        }

        std::filesystem::remove_all(directory);
        std::printf("XML_NATIVE_PASS checks=%u\n", checks);
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "%s\n", failure.what());
        return 1;
    }
}
