#pragma once

#include <utility>

#include <utest/utest.hpp>

#include <dump/operations_mock.hpp>

namespace dump {

/// Converts to binary using `Write(Writer&, const T&)`
template <typename T>
std::string ToBinary(const T& value) {
  MockWriter writer;
  writer.Write(value);
  return std::move(writer).Extract();
}

/// Converts from binary using `Read(Reader&, To<T>)`
template <typename T>
T FromBinary(std::string data) {
  MockReader reader(std::move(data));
  T value = reader.Read<T>();
  reader.Finish();
  return value;
}

/// Write a value to a cache dump and immediately read it back. If `Write` and
/// `Read` are implemented correctly, the result should be equal to the original
/// value.
template <typename T>
void TestWriteReadCycle(const T& value) {
  EXPECT_EQ(FromBinary<T>(ToBinary(value)), value);
}

}  // namespace dump
