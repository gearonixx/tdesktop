#include <QtCore/QBuffer>
#include <QtGui/QImage>
#include "heif_p.h"
#include <cstdio>
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  QByteArray bytes(n,0); fread(bytes.data(),1,n,f); fclose(f);
  QBuffer b(&bytes); b.open(QIODevice::ReadOnly);
  HEIFHandler h; h.setDevice(&b);
  printf("canRead=%d\n", h.canRead());
  QImage img; bool ok=h.read(&img);
  printf("read=%d w=%d h=%d\n", ok, img.width(), img.height());
  return 0;
}
