#include <storages/postgres/exceptions.hpp>
#include <storages/postgres/io/user_types.hpp>

#include <logging/log.hpp>

namespace storages::postgres {

namespace {

using ParserMap = std::unordered_multimap<DBTypeName, std::string>;

ParserMap& BinaryParsers() {
  static ParserMap parsers_;
  return parsers_;
}

ParserMap& TextParsers() {
  static ParserMap parsers_;
  return parsers_;
}

const std::unordered_map<DBTypeDescription::TypeCategory, io::BufferCategory,
                         DBTypeDescription::TypeCategoryHash>
    kTypeCategoryToBufferCategory{
        {DBTypeDescription::TypeCategory::kArray,
         io::BufferCategory::kArrayBuffer},
        {DBTypeDescription::TypeCategory::kComposite,
         io::BufferCategory::kCompositeBuffer},
        {DBTypeDescription::TypeCategory::kRange,
         io::BufferCategory::kRangeBuffer},
    };

io::BufferCategory FindBufferCategory(DBTypeDescription::TypeCategory cat) {
  if (auto f = kTypeCategoryToBufferCategory.find(cat);
      f != kTypeCategoryToBufferCategory.end()) {
    return f->second;
  }
  return io::BufferCategory::kPlainBuffer;
}

}  // namespace

void UserTypes::Reset() {
  types_.clear();
  by_oid_.clear();
  by_name_.clear();
  buffer_categories_.clear();
  composite_types_.clear();
}

Oid UserTypes::FindOid(DBTypeName name) const {
  if (auto f = by_name_.find(name); f != by_name_.end()) {
    return f->second->oid;
  }
  LOG_WARNING() << "PostgreSQL type " << name.schema << "." << name.name
                << " not found";
  return kInvalidOid;
}

Oid UserTypes::FindArrayOid(DBTypeName name) const {
  if (auto f = by_name_.find(name); f != by_name_.end()) {
    return f->second->array_type;
  }
  LOG_WARNING() << "PostgreSQL type " << name.schema << "." << name.name
                << " not found";
  return kInvalidOid;
}

Oid UserTypes::FindElementOid(Oid array_oid) const {
  auto predefined =
      io::GetArrayElementOid(static_cast<io::PredefinedOids>(array_oid));
  if (predefined != io::PredefinedOids::kInvalid) {
    return static_cast<Oid>(predefined);
  }
  if (auto f = by_oid_.find(array_oid); f != by_oid_.end()) {
    if (f->second->category == DBTypeDescription::TypeCategory::kArray) {
      return f->second->element_type;
    }
  }
  return kInvalidOid;
}

DBTypeName UserTypes::FindName(Oid oid) const {
  if (auto f = by_oid_.find(oid); f != by_oid_.end()) {
    return f->second->GetName();
  }
  LOG_WARNING() << "PostgreSQL type with oid " << oid << " not found";
  return {};
}

DBTypeName UserTypes::FindBaseName(Oid oid) const {
  using TypeCategory = DBTypeDescription::TypeCategory;
  using TypeClass = DBTypeDescription::TypeClass;

  if (auto f = by_oid_.find(oid); f != by_oid_.end()) {
    // We have a type description
    const auto& desc = *f->second;
    if (desc.type_class == TypeClass::kDomain) {
      return FindBaseName(desc.base_type);
    } else if (desc.category == TypeCategory::kArray) {
      return FindBaseName(desc.element_type);
    }
    return f->second->GetName();
  }
  LOG_WARNING() << "PostgreSQL type with oid " << oid << " not found";
  return {};
}

Oid UserTypes::FindBaseOid(Oid oid) const {
  using TypeCategory = DBTypeDescription::TypeCategory;
  using TypeClass = DBTypeDescription::TypeClass;
  if (auto f = by_oid_.find(oid); f != by_oid_.end()) {
    // We have a type description
    const auto& desc = *f->second;
    if (desc.type_class == TypeClass::kDomain) {
      return FindBaseOid(desc.base_type);
    } else if (desc.category == TypeCategory::kArray) {
      return FindBaseOid(desc.element_type);
    }
    return desc.oid;
  }
  LOG_WARNING() << "PostgreSQL user type with oid " << oid << " not found";
  return oid;
}

Oid UserTypes::FindBaseOid(DBTypeName name) const {
  auto oid = FindOid(name);
  return FindBaseOid(oid);
}

bool UserTypes::HasBinaryParser(Oid oid) const {
  auto name = FindBaseName(oid);
  if (!name.Empty()) {
    return io::HasBinaryParser(name);
  }
  return false;
}

bool UserTypes::HasTextParser(Oid oid) const {
  auto name = FindBaseName(oid);
  if (!name.Empty()) {
    return io::HasTextParser(name);
  }
  return false;
}

io::BufferCategory UserTypes::GetBufferCategory(Oid oid) const {
  auto cat = io::GetBufferCategory(static_cast<io::PredefinedOids>(oid));
  if (cat != io::BufferCategory::kNoParser) {
    return cat;
  }
  if (auto f = buffer_categories_.find(oid); f != buffer_categories_.end()) {
    return f->second;
  }
  return io::BufferCategory::kNoParser;
}

void UserTypes::AddType(DBTypeDescription&& desc) {
  auto oid = desc.oid;
  LOG_DEBUG() << "User type " << oid << " " << desc.schema << "." << desc.name
              << " class: " << static_cast<char>(desc.type_class)
              << " category: " << static_cast<char>(desc.category);

  if (auto ins = types_.insert(std::move(desc)); ins.second) {
    by_oid_.insert(std::make_pair(oid, ins.first));
    by_name_.insert(std::make_pair(ins.first->GetName(), ins.first));
    auto cat = FindBufferCategory(ins.first->category);
    buffer_categories_.insert(std::make_pair(oid, cat));
  } else {
    // schema and name is not available any more as it was moved
    LOG_ERROR() << "Failed to insert user type with oid " << oid;
  }
}

void UserTypes::AddCompositeFields(CompositeFieldDefs&& defs) {
  if (defs.empty()) {
    return;
  }
  auto p = defs.begin();
  Oid current = p->owner;
  auto start = p;
  for (; p != defs.end(); ++p) {
    // The last group is handled automatically as we have fake element in the
    // end
    if (p->owner != current) {
      // owner changed
      LOG_DEBUG() << "Add " << p - start << " attributes to composite type "
                  << current;
      composite_types_.insert(
          std::make_pair(current, CompositeTypeDescription{start, p}));
      start = p;
      current = p->owner;
    }
  }
}

const CompositeTypeDescription& UserTypes::GetCompositeDescription(
    Oid oid) const {
  if (auto f = composite_types_.find(oid); f != composite_types_.end()) {
    return f->second;
  }
  throw UserTypeError{"Composite type description for oid " +
                      std::to_string(oid) + " not found"};
}

namespace io {

bool HasTextParser(DBTypeName name) { return TextParsers().count(name) > 0; }

bool HasBinaryParser(DBTypeName name) {
  return BinaryParsers().count(name) > 0;
}

namespace detail {

RegisterUserTypeParser RegisterUserTypeParser::Register(
    const DBTypeName& pg_name, std::string&& cpp_name, bool text_parser,
    bool bin_parser) {
  auto both = text_parser && bin_parser;
  if (both) {
    TextParsers().insert(std::make_pair(pg_name, cpp_name));
    BinaryParsers().insert(std::make_pair(pg_name, std::move(cpp_name)));
  } else if (text_parser) {
    TextParsers().insert(std::make_pair(pg_name, std::move(cpp_name)));
  } else if (bin_parser) {
    BinaryParsers().insert(std::make_pair(pg_name, std::move(cpp_name)));
  }
  return {};
}

}  // namespace detail

}  // namespace io

}  // namespace storages::postgres
