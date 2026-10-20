/* WPA2 personal, CCMP, EAPOL-Key descriptor v2 only. Firmware owns 802.11
 * encryption; this bounded supplicant derives/validates the handshake keys.
 * Crypto primitives: unmodified Mbed TLS AES/SHA1 (vendor/mbedtls). */
#include "lume/wpa.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha1.h"
#include <string.h>
const unsigned char lume_wpa_rsn[22]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
void lume_secret_clear(void *p,size_t n){volatile unsigned char *q=p;while(n--)*q++=0;}
void mbedtls_platform_zeroize(void *p,size_t n){lume_secret_clear(p,n);}
static unsigned be16(const unsigned char *p){return (unsigned)p[0]<<8|p[1];}
static void put16(unsigned char *p,unsigned n){p[0]=(unsigned char)(n>>8);p[1]=(unsigned char)n;}
static bool equal(const void *a,const void *b,size_t n)
{const unsigned char *p=a,*q=b;unsigned d=0;for(size_t i=0;i<n;++i)d|=p[i]^q[i];return !d;}
void lume_wpa_hmac(const void *key,size_t kn,const void *data,size_t n,unsigned char out[20])
{
    unsigned char k[64]={0},inner[20];
    if(kn>64){(void)mbedtls_sha1_ret(key,kn,k);kn=20;}else memcpy(k,key,kn);
    for(unsigned i=0;i<64;++i)k[i]^=0x36;
    mbedtls_sha1_context c;mbedtls_sha1_init(&c);(void)mbedtls_sha1_starts_ret(&c);
    (void)mbedtls_sha1_update_ret(&c,k,64);(void)mbedtls_sha1_update_ret(&c,data,n);(void)mbedtls_sha1_finish_ret(&c,inner);
    for(unsigned i=0;i<64;++i)k[i]^=0x36^0x5c;
    (void)mbedtls_sha1_starts_ret(&c);(void)mbedtls_sha1_update_ret(&c,k,64);
    (void)mbedtls_sha1_update_ret(&c,inner,20);(void)mbedtls_sha1_finish_ret(&c,out);
    mbedtls_sha1_free(&c);lume_secret_clear(k,sizeof(k));lume_secret_clear(inner,sizeof(inner));
}
bool lume_wpa_pmk(const void *key,size_t kn,const void *ssid,size_t sn,unsigned char out[32])
{
    if(!key || kn<8 || kn>63 || !ssid || !sn || sn>32 || !out)return false;
    unsigned char salt[36]={0},u[20],v[20];memcpy(salt,ssid,sn);
    for(unsigned block=1;block<=2;++block) {
        salt[sn+3]=(unsigned char)block;lume_wpa_hmac(key,kn,salt,sn+4,u);memcpy(v,u,20);
        for(unsigned i=1;i<4096;++i){lume_wpa_hmac(key,kn,u,20,u);for(unsigned j=0;j<20;++j)v[j]^=u[j];}
        memcpy(out+(block-1)*20,v,block==1?20:12);
    }
    lume_secret_clear(salt,sizeof(salt));lume_secret_clear(u,sizeof(u));lume_secret_clear(v,sizeof(v));return true;
}
bool lume_wpa_unwrap(const unsigned char key[16],const void *input,size_t n,void *output,size_t *length)
{
    if(!key || !input || !output || !length || n<24 || n>384 || (n&7) || *length<n-8)return false;
    const unsigned char *p=input;unsigned char *out=output,a[8],b[16];memcpy(a,p,8);memcpy(out,p+8,n-8);
    mbedtls_aes_context c;mbedtls_aes_init(&c);bool ok=mbedtls_aes_setkey_dec(&c,key,128)==0;
    unsigned blocks=(unsigned)n/8-1;
    for(int j=5;ok && j>=0;--j)for(unsigned i=blocks;i>0;--i) {
        memcpy(b,a,8);unsigned t=blocks*(unsigned)j+i;
        for(unsigned k=0;k<8;++k){b[7-k]^=(unsigned char)t;t>>=8;}
        memcpy(b+8,out+(i-1)*8,8);ok=mbedtls_aes_crypt_ecb(&c,MBEDTLS_AES_DECRYPT,b,b)==0;
        memcpy(a,b,8);memcpy(out+(i-1)*8,b+8,8);
    }
    const unsigned char iv[8]={166,166,166,166,166,166,166,166};ok=ok && equal(a,iv,8);
    mbedtls_aes_free(&c);lume_secret_clear(a,8);lume_secret_clear(b,16);
    if(!ok){lume_secret_clear(out,n-8);return false;}*length=n-8;return true;
}
bool lume_wpa_begin(struct lume_wpa *s,const unsigned char mac[6],const void *ssid,size_t sn,const void *key,size_t kn,bool (*entropy)(void *,size_t))
{
    if(!s || !mac || !entropy)return false;memset(s,0,sizeof(*s));s->entropy=entropy;memcpy(s->mac,mac,6);
    memcpy(s->rsn,lume_wpa_rsn,sizeof(lume_wpa_rsn));s->rsn_bytes=sizeof(lume_wpa_rsn);
    return lume_wpa_pmk(key,kn,ssid,sn,s->pmk);
}
bool lume_wpa_set_rsn(struct lume_wpa *s,const void *ie,size_t n)
{
    const unsigned char *p=ie;
    if(!s || !p || s->seen || n<22 || n>sizeof(s->rsn) || p[0]!=48 || (size_t)p[1]+2!=n ||
       memcmp(p+2,lume_wpa_rsn+2,18) || (p[20]&0x40))return false;
    memcpy(s->rsn,p,n);s->rsn_bytes=n;return true;
}
static void derive(struct lume_wpa *s)
{
    unsigned char data[100]={0},digest[20];const char label[]="Pairwise key expansion";size_t off=sizeof(label);
    memcpy(data,label,sizeof(label));const unsigned char *lo=s->mac,*hi=s->peer;
    if(memcmp(lo,hi,6)>0){lo=s->peer;hi=s->mac;}memcpy(data+off,lo,6);memcpy(data+off+6,hi,6);off+=12;
    lo=s->snonce;hi=s->anonce;if(memcmp(lo,hi,32)>0){lo=s->anonce;hi=s->snonce;}
    memcpy(data+off,lo,32);memcpy(data+off+32,hi,32);off+=64;
    for(unsigned i=0;i<4;++i){data[off]=(unsigned char)i;lume_wpa_hmac(s->pmk,32,data,off+1,digest);memcpy(s->ptk+i*20,digest,i==3?4:20);}
    lume_secret_clear(data,sizeof(data));lume_secret_clear(digest,sizeof(digest));
}
static void reply(struct lume_wpa *s,const unsigned char *in,unsigned info,bool nonce,bool rsn,struct lume_wpa_action *a)
{
    unsigned char *p=a->frame;size_t n=14+4+95+(rsn?s->rsn_bytes:0);memcpy(p,s->peer,6);memcpy(p+6,s->mac,6);
    p[12]=0x88;p[13]=0x8e;p[14]=in[14];p[15]=3;put16(p+16,(unsigned)n-18);p[18]=2;put16(p+19,info);
    memcpy(p+23,in+23,8);if(nonce)memcpy(p+31,s->snonce,32);
    if(rsn){put16(p+111,(unsigned)s->rsn_bytes);memcpy(p+113,s->rsn,s->rsn_bytes);}
    unsigned char mic[20];lume_wpa_hmac(s->ptk,16,p+14,n-14,mic);memcpy(p+95,mic,16);lume_secret_clear(mic,20);a->bytes=n;
}
static bool parse_gtk(struct lume_wpa *s,const unsigned char *data,size_t n,struct lume_wpa_action *a)
{
    bool found=false;
    for(size_t pos=0;pos<n;) {
        if(data[pos]==0xdd && (pos+1==n || data[pos+1]==0))break;
        if(n-pos<2 || data[pos+1]>n-pos-2)return false;size_t len=data[pos+1];const unsigned char *p=data+pos+2;
        if(data[pos]==0xdd && len>=4 && p[0]==0 && p[1]==15 && p[2]==172 && p[3]==1) {
            if(found || len!=22 || (p[4]&0xf8) || p[5])return false;
            found=true;a->gtk_id=p[4]&3;memcpy(a->gtk,p+6,16);
            a->group=!s->gtk_valid || s->gtk_id!=a->gtk_id || !equal(s->gtk,a->gtk,16);
        }
        pos+=len+2;
    }
    if(found && a->group){memcpy(s->gtk,a->gtk,16);s->gtk_id=a->gtk_id;s->gtk_valid=true;}return found;
}
int lume_wpa_input(struct lume_wpa *s,const void *data,size_t bytes,struct lume_wpa_action *a)
{
    const unsigned char *p=data;if(!s || !p || !a || bytes<113)return 0;
    memset(a,0,sizeof(*a));
    if(p[12]!=0x88 || p[13]!=0x8e || p[15]!=3 || p[14]<1 || p[14]>3 || p[18]!=2 || !equal(p,s->mac,6) || (p[6]&1))return 0;
    unsigned body=be16(p+16),info=be16(p+19),keybytes=be16(p+111);
    if(body<95 || body>bytes-18 || body!=95+keybytes || body>479 || (info&7)!=2 || (info&0xec00))return -1;
    bool pair=(info&8)!=0;
    if(pair && (info&0x180)==0x80 && !(info&0x1240)) {
        int order=s->seen?memcmp(p+23,s->replay,8):1;if(order<0)return -1;
        if(order==0) {
            if(!equal(s->peer,p+6,6) || !equal(s->anonce,p+31,32) || s->installed)return -1;
        } else {
            if(!s->entropy(s->snonce,32))return -1;
            memcpy(s->peer,p+6,6);memcpy(s->anonce,p+31,32);memcpy(s->replay,p+23,8);
            s->seen=true;s->installed=s->authorized=false;derive(s);
        }
        reply(s,p,0x10a,true,true,a);return 1;
    }
    if(!s->seen || !equal(s->peer,p+6,6) || (info&0x180)!=0x180 || memcmp(p+23,s->replay,8)<0)return -1;
    unsigned char signed_data[483],mic[20];memcpy(signed_data,p+14,body+4);memset(signed_data+81,0,16);
    lume_wpa_hmac(s->ptk,16,signed_data,body+4,mic);bool valid=equal(mic,p+95,16);
    lume_secret_clear(signed_data,sizeof(signed_data));lume_secret_clear(mic,sizeof(mic));if(!valid)return -1;
    if(pair && (!equal(s->anonce,p+31,32) || (info&0x1240)!=0x1240))return -1;
    if(!pair && (!s->authorized || (info&0x1200)!=0x1200))return -1;
    bool duplicate=memcmp(p+23,s->replay,8)==0 && s->installed;
    if(!duplicate) {
        unsigned char plain[384];size_t len=sizeof(plain);
        if(!lume_wpa_unwrap(s->ptk+16,p+113,keybytes,plain,&len))return -1;
        bool ok=parse_gtk(s,plain,len,a);lume_secret_clear(plain,sizeof(plain));if(!ok)return -1;
        memcpy(a->rsc,p+79,8);
        if(pair && !s->installed){a->pairwise=true;memcpy(a->tk,s->ptk+32,16);s->installed=true;}
        memcpy(s->replay,p+23,8);
    }
    reply(s,p,pair?0x30a:0x302,false,false,a);a->authorize=pair && !s->authorized;s->authorized=true;return 1;
}
