// repl_attributes.cpp — see repl_attributes.h.
#include "solve/repl_attributes.h"

#include "solve/repl_internal.h"

#include <limits>

namespace rbxclient {
using namespace detail;

bool parseA1Attributes(const uint8_t* d, size_t len, uint32_t entity_id,
                       size_t named, const std::vector<uint64_t>& mask_words,
                       A1AttributeSchemas* schemas,
                       std::vector<A1AttributeUpdate>* updates,
                       std::string* err, size_t* consumed, bool* dropped,
                       const uint8_t* guard, size_t guard_len) {
    if (updates)
        updates->clear();
    if (consumed)
        *consumed = 0;
    if (dropped)
        *dropped = false;
    if (!d && len)
        return fail(err, "null attribute payload");
    constexpr uint32_t kNullRef = std::numeric_limits<uint32_t>::max();
    constexpr size_t kMaxAttributeId = 64; // native table bound
    // Attribute values are read from the shared packet stream, so a value may
    // cross the recorded body end exactly like a named member value. Native
    // reads those bytes and then restores v142; give the attribute cursor the
    // same contiguous guard window.
    std::vector<uint8_t> padded;
    const uint8_t* read_base = d;
    size_t read_len = len;
    if (guard_len != 0) {
        padded.assign(d, d + len);
        padded.insert(padded.end(), guard, guard + guard_len);
        read_base = padded.data();
        read_len = padded.size();
    }
    Cursor c(read_base, read_len);
    std::vector<A1AttributeSchema> local_schema;
    auto entity_schema = [&]() -> std::vector<A1AttributeSchema>* {
        if (!schemas)
            return &local_schema;
        return &(*schemas)[entity_id];
    };
    // A schema can be present in an earlier record than its value, so the
    // section is driven by the mask rather than by the tail's first byte.
    bool saw_attribute = false;
    for (size_t wi = 0; wi < mask_words.size(); ++wi) {
        uint64_t bits = mask_words[wi];
        while (bits) {
            const size_t bit = size_t(__builtin_ctzll(bits));
            bits &= bits - 1;
            const size_t idx = wi * 64 + bit;
            if (idx < named)
                continue;
            saw_attribute = true;
            const size_t rel = idx - named;
            const size_t attr_id = rel / 2;
            if (attr_id >= kMaxAttributeId)
                return fail(err, "A1 attribute id " + std::to_string(attr_id) +
                                     " exceeds native table bound at offset " +
                                     std::to_string(c.off()));
            if ((rel & 1) == 0) {
                std::vector<A1AttributeSchema>* per_entity = entity_schema();
                if (per_entity->size() <= attr_id)
                    per_entity->resize(attr_id + 1);
                // Schema entry: [00][tag][u32 name length][name]. The leading
                // zero is a fixed discriminator in every measured frame; a
                // non-zero byte here is a value where a schema was promised.
                uint8_t marker = 0, tag = 0;
                if (!c.u8(&marker, err, "A1 attribute schema marker") ||
                    !c.u8(&tag, err, "A1 attribute schema type"))
                    return false;
                if (marker != 0)
                    return fail(err, "A1 attribute schema marker " +
                                         std::to_string(unsigned(marker)) +
                                         " at offset " +
                                         std::to_string(c.off() - 2));
                A1AttributeSchema s;
                s.tag = tag;
                // Native deserializeAttributeSchema reads an extra Enum name
                // string before the attribute name when the type is 7
                // (sub_643456E: `if (type == 7) readString(...)`). Both use
                // the same u32-length string form as the attribute name.
                auto read_name = [&](std::string* out, const char* what,
                                     bool allow_empty) {
                    const uint8_t* nb = nullptr;
                    if (!c.take(4, &nb, err, what))
                        return false;
                    const uint32_t n = u32le(nb);
                    if ((!allow_empty && n == 0) || n > 1024) {
                        fail(err, std::string("A1 attribute ") + what +
                                      " " + std::to_string(n) +
                                      " at offset " +
                                      std::to_string(c.off() - 4));
                        return false;
                    }
                    if (!c.take(size_t(n), &nb, err, what))
                        return false;
                    out->assign(reinterpret_cast<const char*>(nb), size_t(n));
                    return true;
                };
                if (tag == 7 &&
                    !read_name(&s.enum_name, "enum name", true))
                    return false;
                if (!read_name(&s.name, "name length", false))
                    return false;
                (*per_entity)[attr_id] = std::move(s);
                continue;
            }
            // Value entry: the schema must already be known. Its tag is the
            // only way to size the bytes. Native sub_64356A4 returns an empty
            // variant when its per-entity schema lookup misses, and the
            // deserializer's LABEL_99 branch (sub_6431154) then seeks to the
            // recorded body end instead of reading this value or any later
            // attribute bit.  Do the same: stop here, report the drop, and let
            // the body end-seek account for the remaining bytes.
            std::vector<A1AttributeSchema>* per_entity = nullptr;
            if (schemas) {
                auto it = schemas->find(entity_id);
                if (it != schemas->end())
                    per_entity = &it->second;
            } else {
                per_entity = &local_schema;
            }
            if (!per_entity || attr_id >= per_entity->size() ||
                (*per_entity)[attr_id].name.empty()) {
                if (consumed)
                    *consumed = std::min(c.off(), len);
                if (dropped)
                    *dropped = true;
                return true;
            }
            const A1AttributeSchema& s = (*per_entity)[attr_id];
            A1AttributeUpdate u;
            u.id = uint32_t(attr_id);
            u.tag = s.tag;
            u.enum_name = s.enum_name;
            u.name = s.name;
            if (s.tag == 0) {
                // Type zero is the removal form (SetAttribute(name, nil)):
                // the native attribute store has no zero-width value type.
            } else if (!read_tagged_value(c, s.tag, false,
                                          "attribute " + s.name, u.value,
                                          err)) {
                return false;
            }
            if (updates)
                updates->push_back(std::move(u));
        }
    }
    if (!saw_attribute)
        return true;
    if (consumed)
        *consumed = c.off();
    if (c.off() < len) {
        // Every attribute bit has now consumed its schema/value. Leftover
        // bytes mean the schema table and the mask disagree; applying the
        // values we did decode is still safe, but hiding the excess would
        // make the next record unframeable, so surface it.
        return fail(err, "A1 attribute section left " +
                             std::to_string(len - c.off()) +
                             " bytes at offset " +
                             std::to_string(c.off()));
    }
    return true;
}

} // namespace rbxclient
