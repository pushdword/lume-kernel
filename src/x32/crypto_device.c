/* Kernel-only AES-GCM FIFO driver. Neither userspace addresses nor SPI access
 * occur here. Caller owns the device lease, releases queue_lock and provides
 * bounded private buffers. Readiness waits service preemption checkpoints. */
#include "crypto_device.h"
#include "random.h"
#include <string.h>
#ifdef LUME_CRYPTO_TEST_MMIO
extern uint32_t lume_crypto_mmio_read(uintptr_t);
extern void lume_crypto_mmio_write(uintptr_t,uint32_t);
#else
static uint32_t lume_crypto_mmio_read(uintptr_t p){return *(volatile uint32_t *)p;}
static void lume_crypto_mmio_write(uintptr_t p,uint32_t n){*(volatile uint32_t *)p=n;}
#endif
static uint32_t load_word(const unsigned char *p,unsigned n){
    uint32_t value=0;for(unsigned i=0;i<n;i++)value|=(uint32_t)p[i]<<(8u*i);return value;
}
static void store_word(unsigned char *p,uint32_t word,unsigned n){
    for(unsigned i=0;i<n;i++)p[i]=(unsigned char)(word>>(8u*i));
}
int lume_crypto_device_discover(struct lume_crypto_device *out,uint32_t timer_hz){
    *out=(struct lume_crypto_device){0};uint32_t h[8];uintptr_t table=0xf0002000u;
    for(unsigned i=0;i<8;i++)h[i]=lume_crypto_mmio_read(table+4u*i);
    if(h[0]!=0x54445748u||h[1]!=1||h[2]!=32||h[3]!=32||!h[4]||h[4]>126||
       h[5]!=32u+32u*h[4]||h[6]!=timer_hz||h[7])return -19;
    unsigned found=0;
    for(unsigned i=0;i<h[4];i++){
        uintptr_t p=table+32u+32u*i;uint32_t type=lume_crypto_mmio_read(p),instance=lume_crypto_mmio_read(p+4);
        if(type!=12||instance)continue;
        if(found++)return -22;
        if(lume_crypto_mmio_read(p+8)!=1||lume_crypto_mmio_read(p+12)!=0xf0003000u||
           lume_crypto_mmio_read(p+16)!=140||lume_crypto_mmio_read(p+20)!=UINT32_MAX||
           lume_crypto_mmio_read(p+24)!=timer_hz||lume_crypto_mmio_read(p+28)!=3)return -95;
    }
    if(!found)return -19;
    uintptr_t base=0xf0003000u;
    if(lume_crypto_mmio_read(base)!=0x31434741u||lume_crypto_mmio_read(base+4)!=1||lume_crypto_mmio_read(base+8)!=3)return -95;
    *out=(struct lume_crypto_device){(uint32_t)base,timer_hz,1};return 0;
}
static int wait_status(struct lume_crypto_device *d,unsigned completion,uint32_t *result){
    /* uptime may predate filesystem startup or a long copy. Establish a fresh
     * clock sample before measuring the timeout, just as on every poll batch. */
    lume_crypto_checkpoint();uint32_t started=lume_crypto_now();unsigned polls=0;
    for(;;){
        uint32_t status=lume_crypto_mmio_read(d->base+12u);
        if(status&~31u)return -5;
        if(!(status&1u)&&(!completion||(status&2u))){*result=status;return 0;}
        if(!(++polls&31u)){
            lume_crypto_checkpoint();if((uint32_t)(lume_crypto_now()-started)>1000u)return -110;
        }
    }
}
static int clear_device(struct lume_crypto_device *d){
    lume_crypto_mmio_write(d->base+16u,2);uint32_t status;
    int result=wait_status(d,0,&status);return result<0?result:status?-5:0;
}
int lume_crypto_device_packet(struct lume_crypto_device *d,struct lume_aes_packet *p){
    if(!d->available)return -95;
    if((p->key_bytes!=16&&p->key_bytes!=32)||p->aad_bytes>LUME_AES_AAD_MAX||
       p->bytes>LUME_AES_PACKET_MAX||p->decrypt>1||(!p->data&&p->bytes))return -22;
    int result=clear_device(d);if(result<0){d->available=0;return result;}
    uintptr_t base=d->base;
    lume_crypto_mmio_write(base+20u,p->decrypt|(p->key_bytes==32?2u:0));
    lume_crypto_mmio_write(base+24u,p->bytes);lume_crypto_mmio_write(base+28u,p->aad_bytes);
    for(unsigned i=0;i<p->key_bytes;i+=4)lume_crypto_mmio_write(base+32u+i,load_word(p->key+i,4));
    for(unsigned i=0;i<12;i+=4)lume_crypto_mmio_write(base+64u+i,load_word(p->nonce+i,4));
    for(unsigned i=0;i<p->aad_bytes;i+=4){unsigned n=p->aad_bytes-i;if(n>4)n=4;
        lume_crypto_mmio_write(base+96u+i,load_word(p->aad+i,n));}
    if(p->decrypt)for(unsigned i=0;i<16;i+=4)lume_crypto_mmio_write(base+80u+i,load_word(p->tag+i,4));
    lume_secret_clear(p->key,sizeof p->key);
    for(unsigned i=0;i<p->bytes;i+=4){unsigned n=p->bytes-i;if(n>4)n=4;
        lume_crypto_mmio_write(base+128u,load_word(p->data+i,n));
        if((i&255u)==252u)lume_crypto_checkpoint();}
    lume_crypto_mmio_write(base+16u,1);uint32_t status;
    result=wait_status(d,1,&status);
    if(result>=0){
        if(status==10u)result=-74;
        else if(status!=6u)result=-5;
        else{
            for(unsigned i=0;i<p->bytes;i+=4){unsigned n=p->bytes-i;if(n>4)n=4;
                store_word(p->data+i,lume_crypto_mmio_read(base+132u),n);
                if((i&255u)==252u)lume_crypto_checkpoint();}
            for(unsigned i=0;i<16;i+=4)store_word(p->tag+i,lume_crypto_mmio_read(base+80u+i),4);
            result=0;
        }
    }
    int cleared=clear_device(d);
    if(cleared<0){d->available=0;return cleared;}
    return result;
}
int lume_crypto_device_selftest(struct lume_crypto_device *d){
    static const unsigned char ciphertext[2][16]={
        {0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78},
        {0xce,0xa7,0x40,0x3d,0x4d,0x60,0x6b,0x6e,0x07,0x4e,0xc5,0xd3,0xba,0xf3,0x9d,0x18}};
    static const unsigned char tags[2][16]={
        {0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf},
        {0xd0,0xd1,0xc8,0xa7,0x99,0x99,0x6b,0xf0,0x26,0x5b,0x98,0xb5,0xd4,0x8a,0xb9,0x19}};
    int result=0;unsigned char data[16];
    for(unsigned variant=0;variant<2;variant++){
        struct lume_aes_packet p={.key_bytes=variant?32:16,.bytes=16,.data=data};memset(data,0,sizeof data);
        result=lume_crypto_device_packet(d,&p);
        if(result<0||memcmp(data,ciphertext[variant],16)||memcmp(p.tag,tags[variant],16)){result=-5;lume_secret_clear(&p,sizeof p);break;}
        p.decrypt=1;result=lume_crypto_device_packet(d,&p);
        unsigned nonzero=0;for(unsigned i=0;i<16;i++)nonzero|=data[i];
        if(result<0||nonzero){result=-5;lume_secret_clear(&p,sizeof p);break;}
        memcpy(data,ciphertext[variant],16);p.tag[0]^=1;result=lume_crypto_device_packet(d,&p);
        if(result!=-74||memcmp(data,ciphertext[variant],16)){result=-5;lume_secret_clear(&p,sizeof p);break;}
        result=0;lume_secret_clear(&p,sizeof p);
    }
    lume_secret_clear(data,sizeof data);if(result<0)d->available=0;return result;
}
#include "crypto_curve_device.inc"
