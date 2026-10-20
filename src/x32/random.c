/* SHA-256 / HMAC_DRBG, SP800-90A revision1 sections10.1.2.2..5.
 * This implements a deterministic generator, not an entropy source and not
 * a claim of NIST validation. Caller serializes access and supplies entropy. */
#include "random.h"
#include <string.h>
struct sha {uint32_t h[8];uint64_t bytes;unsigned used;unsigned char block[64];};
static const uint32_t round_k[64]={
 0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
 0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
 0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
 0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
 0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
 0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
 0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
 0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
void lume_secret_clear(void *memory,unsigned n){volatile unsigned char *p=memory;while(n--)*p++=0;}
static uint32_t ror(uint32_t n,unsigned bits){return (n>>bits)|(n<<(32u-bits));}
static void transform(struct sha *s){
    uint32_t w[64];for(unsigned i=0;i<16;i++){const unsigned char *p=s->block+4*i;
        w[i]=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
    for(unsigned i=16;i<64;i++){
        uint32_t a=w[i-15],b=w[i-2];
        w[i]=w[i-16]+(ror(a,7)^ror(a,18)^(a>>3))+w[i-7]+(ror(b,17)^ror(b,19)^(b>>10));
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for(unsigned i=0;i<64;i++){
        uint32_t t1=h+(ror(e,6)^ror(e,11)^ror(e,25))+((e&f)^(~e&g))+round_k[i]+w[i];
        uint32_t t2=(ror(a,2)^ror(a,13)^ror(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
    lume_secret_clear(w,sizeof w);
    lume_random_checkpoint();
}
static void sha_start(struct sha *s){
    *s=(struct sha){.h={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u}};
}
static void sha_add(struct sha *s,const void *data,unsigned n){
    const unsigned char *p=data;s->bytes+=n;
    while(n){unsigned take=64-s->used;if(take>n)take=n;memcpy(s->block+s->used,p,take);
        s->used+=take;p+=take;n-=take;if(s->used==64){transform(s);s->used=0;}}
}
static void sha_end(struct sha *s,unsigned char out[32]){
    uint64_t bits=s->bytes*8u;unsigned char pad[128]={0x80};
    unsigned n=s->used<56?56-s->used:120-s->used;sha_add(s,pad,n);
    for(unsigned i=0;i<8;i++)pad[i]=(unsigned char)(bits>>(56u-8u*i));sha_add(s,pad,8);
    for(unsigned i=0;i<32;i++)out[i]=(unsigned char)(s->h[i/4]>>(24u-(i%4)*8u));
    lume_secret_clear(pad,sizeof pad);lume_secret_clear(s,sizeof *s);
}
void lume_sha256(const void *data,unsigned n,unsigned char out[32]){
    struct sha s;sha_start(&s);sha_add(&s,data,n);sha_end(&s,out);
}
void lume_hmac_sha256(const unsigned char key[32],const void *a,unsigned an,const void *b,unsigned bn,unsigned char out[32]){
    unsigned char pad[64],hash[32];struct sha s;
    for(unsigned i=0;i<64;i++)pad[i]=(i<32?key[i]:0)^0x36u;
    sha_start(&s);sha_add(&s,pad,64);sha_add(&s,a,an);sha_add(&s,b,bn);sha_end(&s,hash);
    for(unsigned i=0;i<64;i++)pad[i]=(i<32?key[i]:0)^0x5cu;
    sha_start(&s);sha_add(&s,pad,64);sha_add(&s,hash,32);sha_end(&s,out);
    lume_secret_clear(pad,sizeof pad);lume_secret_clear(hash,sizeof hash);
}
static void update(struct lume_random_state *s,const unsigned char *seed,unsigned n){
    unsigned char value[33];memcpy(value,s->value,32);value[32]=0;
    lume_hmac_sha256(s->key,value,33,seed,n,s->key);
    lume_hmac_sha256(s->key,s->value,32,0,0,s->value);
    if(n){memcpy(value,s->value,32);value[32]=1;
        lume_hmac_sha256(s->key,value,33,seed,n,s->key);
        lume_hmac_sha256(s->key,s->value,32,0,0,s->value);}
    lume_secret_clear(value,sizeof value);
}
int lume_random_seed(struct lume_random_state *s,const unsigned char *seed,unsigned n){
    if(n!=LUME_RANDOM_SEED_BYTES||!seed)return -22;
    if(!s->ready){memset(s->key,0,32);memset(s->value,1,32);}
    update(s,seed,n);s->ready=1;s->remaining=LUME_RANDOM_BUDGET;s->reseeds++;return 0;
}
int lume_random_bytes(struct lume_random_state *s,void *data,unsigned n){
    if(n>LUME_RANDOM_MAX||(!data&&n))return -22;if(!n)return 0;
    if(!s->ready||n>s->remaining)return -11;
    unsigned char *p=data;unsigned left=n;
    while(left){lume_hmac_sha256(s->key,s->value,32,0,0,s->value);
        unsigned take=left<32?left:32;memcpy(p,s->value,take);p+=take;left-=take;}
    update(s,0,0);s->remaining-=n;return (int)n;
}
