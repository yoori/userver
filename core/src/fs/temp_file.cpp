#include <userver/fs/temp_file.hpp>

#include <userver/engine/async.hpp>

USERVER_NAMESPACE_BEGIN

namespace fs {

// Please, review this TempFile::TempFile implementation
// I simple added it as workaround for linkage error (if linked as shared library)
// now project don't use TempFile
TempFile::TempFile(userver::engine::TaskProcessor& fs_task_processor,
  userver::fs::blocking::TempFile temp_file)
  : fs_task_processor_(fs_task_processor),
    temp_file_(std::move(temp_file))
{
}

TempFile TempFile::Create(engine::TaskProcessor& fs_task_processor) {
  return {
      fs_task_processor,
      engine::AsyncNoSpan(fs_task_processor,
                          [] { return blocking::TempFile::Create(); })
          .Get(),
  };
}

TempFile TempFile::Create(std::string_view parent_path,
                          std::string_view name_prefix,
                          engine::TaskProcessor& fs_task_processor) {
  return {
      fs_task_processor,
      engine::AsyncNoSpan(fs_task_processor,
                          [&parent_path, &name_prefix] {
                            return blocking::TempFile::Create(parent_path,
                                                              name_prefix);
                          })
          .Get(),
  };
}

TempFile::~TempFile() { std::move(*this).Remove(); }

TempFile TempFile::Adopt(std::string path,
                         engine::TaskProcessor& fs_task_processor) {
  return {fs_task_processor, blocking::TempFile::Adopt(path)};
}

const std::string& TempFile::GetPath() const { return temp_file_.GetPath(); }

void TempFile::Remove() && {
  engine::AsyncNoSpan(fs_task_processor_, [this] {
    std::move(temp_file_).Remove();
  }).Get();
}

}  // namespace fs

USERVER_NAMESPACE_END
