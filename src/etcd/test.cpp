#include <cassert>
#include <iostream>

#include "etcdapi.hpp"

using namespace etcdapi;
static constexpr const char* kAddress = "http://127.0.0.1:2379";

int main(int argc, char** argv) {
  auto client = create_v3_client(kAddress);

  etcdapi::status_code_e code;

  code = client->kv_put("asdf", "asdfasdf2", 0);
  assert(code == etcdapi::status_code_e::OK);
  auto v = client->kv_get("asdf");
  assert(v.is_ok());
  std::cout << v.value << std::endl;

  auto d = client->kv_delete("asdf");
  assert(d == etcdapi::status_code_e::OK);

  v = client->kv_get("asdf");
  assert(!v.is_ok());

  return 0;
}