#include "latex.h"
#include <cstdio>
#include <vector>
#include <string>
using namespace tex;
int main(int argc,char**argv){
  LaTeX::initBundled();
  FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
  std::vector<unsigned char> b(n);fread(b.data(),1,n,f);fclose(f);
  std::wstring w; for(unsigned char c: b) w.push_back((wchar_t)c);
  try{ delete LaTeX::parse(w,720,20.f,6.f,0xFF000000);}catch(...){}
  printf("ok\n");return 0;
}
