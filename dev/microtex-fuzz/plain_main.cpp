#include "latex.h"
#include <string>
#include <cstdio>
using namespace tex;
int main(int argc, char** argv){
  LaTeX::initBundled();
  std::string s = argc>1?argv[1]:"\\color{red}{x}";
  std::wstring w(s.begin(), s.end());
  try {
    TeXRender* r = LaTeX::parse(w, 720, 20.f, 6.f, 0xFF000000);
    delete r;
    printf("OK: parsed [%s]\n", s.c_str());
  } catch(std::exception& e){ printf("EXC: %s\n", e.what()); }
  catch(...){ printf("EXC unknown\n"); }
  return 0;
}
