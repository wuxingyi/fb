#include <cassert>
#include <iostream>

#include "etcdapi.h"

using namespace Etcd;
static constexpr const char* kAddress = "http://127.0.0.1:2379";

int main(int argc, char** argv) {
  auto client = Client_Create(kAddress);

  Etcd::StatusCode code;

  code = client->Put("asdf", "asdfasdf2", 0);
  assert(code == Etcd::StatusCode::Ok);
  auto v = client->Get("asdf");
  assert(v.IsOk());
  std::cout << v.value << std::endl;

  auto d = client->Delete("asdf");
  assert(d == Etcd::StatusCode::Ok);

  v = client->Get("asdf");
  assert(!v.IsOk());

  return 0;
}