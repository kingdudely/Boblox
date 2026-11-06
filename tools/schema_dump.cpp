// schema_dump.cpp — print a capture's 0x97 schema as TSV.
//
// Usage: schema_dump <chan1.bin>   (> schema.tsv)
//
// Columns: prop_id  class  member  tag  prefix
// prop_id is the native global id (running index over classes and their
// members in schema order), so this table is directly comparable with
// tools/proptable.tsv and with an API-dump type check.
#include "solve/replicate.h"
#include "repl_toolkit.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: schema_dump <chan1.bin>\n");
        return 2;
    }
    const std::vector<uint8_t> d = repltool::slurpFile(argv[1]);
    const std::vector<repltool::CapMsg> msgs = repltool::splitFrames(d);
    std::vector<uint8_t> inner;
    std::string err;
    if (!repltool::extractInner97(msgs, inner, &err)) {
        std::fprintf(stderr, "no 0x97: %s\n", err.c_str());
        return 2;
    }
    rbxclient::JoinSchema schema;
    if (!rbxclient::parseJoinSchema(inner.data(), inner.size(), schema, &err)) {
        std::fprintf(stderr, "schema: %s\n", err.c_str());
        return 2;
    }
    const rbxclient::PropTagTable t = rbxclient::propTagsForSchema(schema);
    for (const auto& kv : t.tag) {
        const char* cls = "?";
        auto own = t.owner_class.find(kv.first);
        if (own != t.owner_class.end() && own->second < schema.classes.size())
            cls = schema.classes[own->second].name.c_str();
        auto nm = t.member_name.find(kv.first);
        std::printf("%u\t%s\t%s\t%u\t%d\n", unsigned(kv.first), cls,
                    nm != t.member_name.end() ? nm->second.c_str() : "?",
                    unsigned(kv.second), t.prefix.count(kv.first) ? 1 : 0);
    }
    return 0;
}
