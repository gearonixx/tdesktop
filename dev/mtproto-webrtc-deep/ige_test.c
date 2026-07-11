#include <openssl/aes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char**argv){
    unsigned char key[32]; memset(key,1,32);
    unsigned char iv[32];  memset(iv,2,32);
    unsigned char in[64];  memset(in,3,64);
    unsigned char out[64]; memset(out,0,64);
    AES_KEY aes; AES_set_decrypt_key(key,256,&aes);
    unsigned len = (argc>1)?(unsigned)atoi(argv[1]):24u;
    fprintf(stderr,"calling AES_ige_encrypt len=%u (len%%16=%u)\n", len, len%16);
    fflush(stderr);
    AES_ige_encrypt(in,out,len,&aes,iv,AES_DECRYPT);
    fprintf(stderr,"RETURNED normally (no abort). out=%02x %02x %02x %02x\n",out[0],out[1],out[2],out[3]);
    return 0;
}
