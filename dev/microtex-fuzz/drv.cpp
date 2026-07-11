#include <cstdint>
#include <cstdio>
#include <vector>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t);
extern "C" int LLVMFuzzerInitialize(int*, char***);
int main(int argc, char** argv){
  LLVMFuzzerInitialize(&argc, &argv);
  FILE* f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  std::vector<uint8_t> b(n); fread(b.data(),1,n,f); fclose(f);
  LLVMFuzzerTestOneInput(b.data(), b.size());
  printf("survived\n"); return 0;
}
