/* LumeOS SD SPI transport, derived from the project read-only driver.
 * CMD24/CMD25 writes, completion/status and readback belong only to the OS. */
#include "sd.h"
#include "clock_io.h"
#ifdef LUME_SD_TEST_MMIO
extern uint32_t lume_hal_read32(uintptr_t);
extern void lume_hal_write32(uintptr_t,uint32_t);
#else
static uint32_t lume_hal_read32(uintptr_t p){return *(volatile uint32_t *)p;}
static void lume_hal_write32(uintptr_t p,uint32_t v){*(volatile uint32_t *)p=v;}
#endif

static int transfer(uintptr_t base,unsigned char tx) {
    lume_hal_write32(base+8u,tx);
    unsigned budget=20000;
    while(lume_hal_read32(base+12u)&1u){
        if(!--budget){io_clock_checkpoint();return -1;}
        /* Only slow/stuck hardware takes this branch. Keep uptime alive even
         * if a byte consumes the entire polling budget with IRQs disabled. */
        if(!(budget&1023u))io_clock_checkpoint();
    }
    return (int)(lume_hal_read32(base+8u)&255u);
}
static void deselect(uintptr_t base) {
    if(lume_hal_read32(base+12u)&1u)lume_hal_write32(base+20u,1); // Abort a timed-out byte before changing CS.
    lume_hal_write32(base+4u,1);
    if(transfer(base,255)<0)lume_hal_write32(base+20u,1);
}
static int command(uintptr_t base,unsigned cmd,uint32_t arg,unsigned crc) {
    deselect(base);lume_hal_write32(base+4u,0);
    if(transfer(base,255)<0||transfer(base,(unsigned char)(64u|cmd))<0)return -1;
    for(unsigned shift=32;shift;) {
        shift-=8;if(transfer(base,(unsigned char)(arg>>shift))<0)return -1;
    }
    if(transfer(base,(unsigned char)crc)<0)return -1;
    for(unsigned count=0;count<8;++count) {
        int byte=transfer(base,255);if(byte<0||!(byte&128)) {
#ifdef LUME_SD_TRACE
            extern void lume_sd_trace(unsigned,uint32_t,int);
            lume_sd_trace(cmd,arg,byte);
#endif
            return byte;
        }
    }
#ifdef LUME_SD_TRACE
    extern void lume_sd_trace(unsigned,uint32_t,int);
    lume_sd_trace(cmd,arg,-1);
#endif
    return -1;
}
/* CRC16-CCITT lookup keeps the serialized SD path bounded without spending
 * eight shift loops on every byte. Table generated from polynomial0x1021. */
static const uint16_t crc_table[256]={
    0x0000u,0x1021u,0x2042u,0x3063u,0x4084u,0x50a5u,0x60c6u,0x70e7u,
    0x8108u,0x9129u,0xa14au,0xb16bu,0xc18cu,0xd1adu,0xe1ceu,0xf1efu,
    0x1231u,0x0210u,0x3273u,0x2252u,0x52b5u,0x4294u,0x72f7u,0x62d6u,
    0x9339u,0x8318u,0xb37bu,0xa35au,0xd3bdu,0xc39cu,0xf3ffu,0xe3deu,
    0x2462u,0x3443u,0x0420u,0x1401u,0x64e6u,0x74c7u,0x44a4u,0x5485u,
    0xa56au,0xb54bu,0x8528u,0x9509u,0xe5eeu,0xf5cfu,0xc5acu,0xd58du,
    0x3653u,0x2672u,0x1611u,0x0630u,0x76d7u,0x66f6u,0x5695u,0x46b4u,
    0xb75bu,0xa77au,0x9719u,0x8738u,0xf7dfu,0xe7feu,0xd79du,0xc7bcu,
    0x48c4u,0x58e5u,0x6886u,0x78a7u,0x0840u,0x1861u,0x2802u,0x3823u,
    0xc9ccu,0xd9edu,0xe98eu,0xf9afu,0x8948u,0x9969u,0xa90au,0xb92bu,
    0x5af5u,0x4ad4u,0x7ab7u,0x6a96u,0x1a71u,0x0a50u,0x3a33u,0x2a12u,
    0xdbfdu,0xcbdcu,0xfbbfu,0xeb9eu,0x9b79u,0x8b58u,0xbb3bu,0xab1au,
    0x6ca6u,0x7c87u,0x4ce4u,0x5cc5u,0x2c22u,0x3c03u,0x0c60u,0x1c41u,
    0xedaeu,0xfd8fu,0xcdecu,0xddcdu,0xad2au,0xbd0bu,0x8d68u,0x9d49u,
    0x7e97u,0x6eb6u,0x5ed5u,0x4ef4u,0x3e13u,0x2e32u,0x1e51u,0x0e70u,
    0xff9fu,0xefbeu,0xdfddu,0xcffcu,0xbf1bu,0xaf3au,0x9f59u,0x8f78u,
    0x9188u,0x81a9u,0xb1cau,0xa1ebu,0xd10cu,0xc12du,0xf14eu,0xe16fu,
    0x1080u,0x00a1u,0x30c2u,0x20e3u,0x5004u,0x4025u,0x7046u,0x6067u,
    0x83b9u,0x9398u,0xa3fbu,0xb3dau,0xc33du,0xd31cu,0xe37fu,0xf35eu,
    0x02b1u,0x1290u,0x22f3u,0x32d2u,0x4235u,0x5214u,0x6277u,0x7256u,
    0xb5eau,0xa5cbu,0x95a8u,0x8589u,0xf56eu,0xe54fu,0xd52cu,0xc50du,
    0x34e2u,0x24c3u,0x14a0u,0x0481u,0x7466u,0x6447u,0x5424u,0x4405u,
    0xa7dbu,0xb7fau,0x8799u,0x97b8u,0xe75fu,0xf77eu,0xc71du,0xd73cu,
    0x26d3u,0x36f2u,0x0691u,0x16b0u,0x6657u,0x7676u,0x4615u,0x5634u,
    0xd94cu,0xc96du,0xf90eu,0xe92fu,0x99c8u,0x89e9u,0xb98au,0xa9abu,
    0x5844u,0x4865u,0x7806u,0x6827u,0x18c0u,0x08e1u,0x3882u,0x28a3u,
    0xcb7du,0xdb5cu,0xeb3fu,0xfb1eu,0x8bf9u,0x9bd8u,0xabbbu,0xbb9au,
    0x4a75u,0x5a54u,0x6a37u,0x7a16u,0x0af1u,0x1ad0u,0x2ab3u,0x3a92u,
    0xfd2eu,0xed0fu,0xdd6cu,0xcd4du,0xbdaau,0xad8bu,0x9de8u,0x8dc9u,
    0x7c26u,0x6c07u,0x5c64u,0x4c45u,0x3ca2u,0x2c83u,0x1ce0u,0x0cc1u,
    0xef1fu,0xff3eu,0xcf5du,0xdf7cu,0xaf9bu,0xbfbau,0x8fd9u,0x9ff8u,
    0x6e17u,0x7e36u,0x4e55u,0x5e74u,0x2e93u,0x3eb2u,0x0ed1u,0x1ef0u,
};
static uint16_t crc_byte(uint16_t crc,unsigned char byte){
    return (uint16_t)((crc<<8)^crc_table[(crc>>8)^byte]);
}
/* Same RX block protocol as Genisis. STATUS is safe on legacy hardware;
 * offsets24/28 must never be touched without the capability bit and magic. */
#ifdef _MSC_VER
typedef uint32_t sd_alias_word;
#else
typedef uint32_t sd_alias_word __attribute__((__may_alias__));
#endif
static int block_capable(uintptr_t base){
    return (lume_hal_read32(base+LUME_SPI_STATUS)&LUME_SPI_CAP_RX_BLOCK)&&
        lume_hal_read32(base+LUME_SPI_RX_BLOCK)==LUME_SPI_RX_MAGIC;
}
/* CSD TRAN_SPEED: reject reserved encodings; unit1MHz needs mantissa5.0. */
static int supports_spi5(const unsigned char *csd){
    unsigned speed=csd[3],unit=speed&7u,value=(speed>>3)&15u;
    return !(speed&128u)&&value&&unit<4u&&
           (unit>=2u||(unit==1u&&value>=11u));
}
static int block_error(uintptr_t base,int status){
    lume_hal_write32(base+LUME_SPI_ABORT,1u);return status;
}
static int receive_block(uintptr_t base,unsigned char *data,unsigned bytes){
    uint32_t status=lume_hal_read32(base+LUME_SPI_STATUS);
    if((status&LUME_SPI_BUSY)||LUME_SPI_WORDS(status)||!bytes||bytes>512u)
        return block_error(base,LUME_SD_TRANSPORT);
    lume_hal_write32(base+LUME_SPI_RX_BLOCK,LUME_SPI_RX_START|bytes);
    unsigned received=0,budget=200000;
    while(received<bytes&&budget--){
        if(!(budget&1023u))io_clock_checkpoint();
        status=lume_hal_read32(base+LUME_SPI_STATUS);
        unsigned words=LUME_SPI_WORDS(status);
        if(words>(bytes-received+3u)/4u)return block_error(base,LUME_SD_TRANSPORT);
        if(!words){
            if(status&LUME_SPI_DONE)return block_error(base,LUME_SD_TRANSPORT);
            continue;
        }
        while(words--){
            uint32_t word=lume_hal_read32(base+LUME_SPI_RX_POP);
            unsigned remaining=bytes-received;
            if(remaining>=4u&&!((uintptr_t)(data+received)&3u)){
                *(sd_alias_word *)(void *)(data+received)=word;received+=4u;
            }else{
                unsigned take=remaining<4u?remaining:4u;
                for(unsigned byte=0;byte<take;byte++)data[received++]=(unsigned char)(word>>(byte*8u));
            }
        }
    }
    if(received!=bytes)return block_error(base,LUME_SD_TIMEOUT);
    budget=200000;
    while(budget--){
        if(!(budget&1023u))io_clock_checkpoint();
        status=lume_hal_read32(base+LUME_SPI_STATUS);
        if(status&LUME_SPI_DONE){
            if((status&LUME_SPI_BUSY)||LUME_SPI_WORDS(status))return block_error(base,LUME_SD_TRANSPORT);
            // Idle/drained CRC failure leaves CS selected for CMD12 cleanup.
            return status&LUME_SPI_CRC_OK?LUME_SD_READY:LUME_SD_CRC;
        }
    }
    return block_error(base,LUME_SD_TIMEOUT);
}
static int ready(uintptr_t base){
    uint32_t start=lume_hal_read32(0xf0007024u),hz=lume_hal_read32(base+16u);
    if(!hz)return 0;
    for(unsigned n=0;n<200000u;n++){
        int v=transfer(base,255);if(v==255)return 1;if(v<0)return 0;
        if(!(n&31u))io_clock_checkpoint();
        if((uint32_t)(lume_hal_read32(0xf0007024u)-start)>=hz)return 0;
    }return 0;
}
int lume_sd_sync(uintptr_t base){
    deselect(base);lume_hal_write32(base+4u,0);
    int ok=ready(base);deselect(base);return ok;
}
static int transmit_payload(uintptr_t base,const unsigned char *data){
    uint32_t flags=lume_hal_read32(base+LUME_SPI_STATUS);
    if((flags&(LUME_SPI_CAP_RX_BLOCK|LUME_SPI_CAP_TX_BLOCK))==
            (LUME_SPI_CAP_RX_BLOCK|LUME_SPI_CAP_TX_BLOCK)&&block_capable(base)){
        if((flags&LUME_SPI_BUSY)||LUME_SPI_WORDS(flags))return block_error(base,LUME_SD_TRANSPORT);
        for(unsigned i=0;i<512;i+=4){
            uint32_t word;
            if(!((uintptr_t)(data+i)&3u))word=*(const sd_alias_word *)(const void *)(data+i);
            else word=(uint32_t)data[i]|((uint32_t)data[i+1]<<8)|((uint32_t)data[i+2]<<16)|((uint32_t)data[i+3]<<24);
            lume_hal_write32(base+LUME_SPI_RX_POP,word);
        }
        lume_hal_write32(base+LUME_SPI_RX_BLOCK,LUME_SPI_TX_START|512u);
        for(unsigned budget=200000;budget;budget--){
            if(!(budget&1023u))io_clock_checkpoint();
            flags=lume_hal_read32(base+LUME_SPI_STATUS);
            if(flags&LUME_SPI_DONE)return !(flags&LUME_SPI_BUSY)&&!LUME_SPI_WORDS(flags)?LUME_SD_READY:block_error(base,LUME_SD_TRANSPORT);
        }
        return block_error(base,LUME_SD_TIMEOUT);
    }
    uint16_t crc=0;
    for(unsigned i=0;i<512;i++){
        crc=crc_byte(crc,data[i]);if(transfer(base,data[i])<0)return LUME_SD_TRANSPORT;
        if((i&127u)==127u)io_clock_checkpoint();
    }
    if(transfer(base,(unsigned char)(crc>>8))<0||transfer(base,(unsigned char)crc)<0)return LUME_SD_TRANSPORT;
    return LUME_SD_READY;
}
uint32_t lume_sd_partial_blocks;
static int data_response(uintptr_t base){
    int response=255;unsigned budget=8;
    do{response=transfer(base,255);}while(response==255&&--budget);
    if(response<0)return LUME_SD_TRANSPORT;
    if((response&31)==13)return LUME_SD_WRITE_ERROR;
    if((response&31)!=5)return LUME_SD_RESPONSE;
    return ready(base)?LUME_SD_READY:LUME_SD_TIMEOUT;
}
static int verify_written(uintptr_t,const struct lume_sd_card *,uint32_t,uint32_t,const unsigned char *);
static int stop_read(uintptr_t);
static void query_written(uintptr_t,uint32_t);
int lume_sd_write_blocks(uintptr_t base,const struct lume_sd_card *card,uint32_t block,uint32_t count,const unsigned char *data){
    if(card->status!=LUME_SD_READY||block>=card->blocks||count>card->blocks-block||count>UINT32_MAX/512u||
       (!card->high_capacity&&(block>UINT32_MAX/512u||(count&&count-1u>UINT32_MAX/512u-block))))return LUME_SD_RESPONSE;
    if(!count)return LUME_SD_READY;
    if(!data||(uintptr_t)data>UINTPTR_MAX-(uintptr_t)count*512u)return LUME_SD_RESPONSE;
    int status=LUME_SD_TRANSPORT,multi=count>1,rejected=0;lume_sd_partial_blocks=UINT32_MAX;
    if(!lume_sd_sync(base))return LUME_SD_TIMEOUT;
    if(command(base,multi?25u:24u,card->high_capacity?block:block*512u,1)!=0)goto done;
    for(uint32_t i=0;i<count;i++){
        if(transfer(base,255)<0||transfer(base,multi?252u:254u)<0){status=LUME_SD_TRANSPORT;break;}
        status=transmit_payload(base,data+i*512u);if(status!=LUME_SD_READY)break;
        status=data_response(base);io_clock_checkpoint();
        if(status!=LUME_SD_READY){rejected=status==LUME_SD_RESPONSE||status==LUME_SD_WRITE_ERROR;break;}
    }
    if(multi){
        /* SPI7.3.3.1 requires CMD12 after a rejected data response. FD is
         * the normal successful CMD25 terminator, not its error recovery. */
        int stopped=0;
        if(rejected)stopped=stop_read(base)==LUME_SD_READY;
        else if(!lume_hal_read32(base+4u)&&ready(base)&&transfer(base,253u)>=0)stopped=ready(base);
        if(!stopped&&status==LUME_SD_READY)status=LUME_SD_TIMEOUT;
        if(stopped&&status==LUME_SD_WRITE_ERROR){
            if(command(base,13,0,1)==0)(void)transfer(base,255);
            query_written(base,count);
        }
    }
    if(status!=LUME_SD_READY)goto done;
    if(command(base,13,0,1)!=0||transfer(base,255)!=0){status=LUME_SD_RESPONSE;goto done;}
    deselect(base);
    status=verify_written(base,card,block,count,data);if(status==LUME_SD_READY)lume_sd_partial_blocks=count;
done:
    deselect(base);return status;
}
int lume_sd_write(uintptr_t base,const struct lume_sd_card *card,uint32_t block,const unsigned char *data){
    return lume_sd_write_blocks(base,card,block,1,data);
}
static int receive(uintptr_t base,unsigned char *data,unsigned bytes) {
    unsigned budget=10000;
    int byte;
    do { byte=transfer(base,255);if(byte<0)return LUME_SD_TRANSPORT;
        if(!(budget&255u))io_clock_checkpoint(); }
    while(byte==255&&--budget);
    if(byte!=254)return budget?LUME_SD_RESPONSE:LUME_SD_TIMEOUT;
    if(block_capable(base))return receive_block(base,data,bytes);
    uint16_t crc=0;
    for(unsigned i=0;i<bytes;++i) {
        byte=transfer(base,255);if(byte<0)return LUME_SD_TRANSPORT;
        data[i]=(unsigned char)byte;crc=crc_byte(crc,data[i]);
        if((i&127u)==127u)io_clock_checkpoint();
    }
    int high=transfer(base,255),low=transfer(base,255);
    if(high<0||low<0)return LUME_SD_TRANSPORT;
    return crc==(uint16_t)((unsigned)high*256u+(unsigned)low)?LUME_SD_READY:LUME_SD_CRC;
}
int lume_sd_decode_csd(const unsigned char *csd,struct lume_sd_card *card) {
    unsigned version=csd[0]>>6;
    uint32_t blocks;
    if(version==1) {
        uint32_t size=((uint32_t)(csd[7]&63u)<<16)|((uint32_t)csd[8]<<8)|csd[9];
        /* Exclude the 2 TiB endpoint that cannot be represented as block count
         * in this provisional 32-bit block API. Do not let it wrap to zero. */
        if(size==0x3fffffu||!card->high_capacity)return 0;
        blocks=(size+1u)<<10;
    } else if(version==0) {
        unsigned read_length=csd[5]&15u;
        unsigned mult=((csd[9]&3u)<<1)|(csd[10]>>7);
        uint32_t size=((uint32_t)(csd[6]&3u)<<10)|((uint32_t)csd[7]<<2)|(csd[8]>>6);
        if(card->high_capacity||read_length<9||read_length>11)return 0;
        blocks=(size+1u)<<(mult+2u+read_length-9u);
    } else return 0;
    if(!blocks)return 0;
    card->blocks=blocks;card->capacity_mib=blocks>>11;
    return 1;
}
int lume_sd_read(uintptr_t base,const struct lume_sd_card *card,uint32_t block,unsigned char *data) {
    if(card->status!=LUME_SD_READY||block>=card->blocks||
       (!card->high_capacity&&block>UINT32_MAX/512u))return LUME_SD_RESPONSE;
    int status=LUME_SD_RESPONSE;
    if(command(base,17,card->high_capacity?block:block*512u,1)==0)status=receive(base,data,512);
    deselect(base);return status;
}
static int stop_read(uintptr_t base){
    if(lume_hal_read32(base+LUME_SPI_STATUS)&LUME_SPI_BUSY)
        lume_hal_write32(base+LUME_SPI_ABORT,1u);
    /* A transport ABORT releases CS; select again before the STOP frame. */
    lume_hal_write32(base+4u,0);
    if(transfer(base,0x4cu)<0)return LUME_SD_TRANSPORT;
    for(unsigned i=0;i<4;i++)if(transfer(base,0)<0)return LUME_SD_TRANSPORT;
    // CMD12 has one mandatory stuff byte before its R1 response.
    if(transfer(base,1)<0||transfer(base,255)<0)return LUME_SD_TRANSPORT;
    int response=255;unsigned budget=8;
    do{
        response=transfer(base,255);
        if(response<0)return LUME_SD_TRANSPORT;
        if(!(response&128))break;
    }while(--budget);
    if(response!=0)return budget?LUME_SD_RESPONSE:LUME_SD_TIMEOUT;
    budget=100000;
    do{
        if(!(budget&255u))io_clock_checkpoint();
        int byte=transfer(base,255);
        if(byte<0)return LUME_SD_TRANSPORT;
        if(byte==255)return LUME_SD_READY;
    }while(--budget);
    return LUME_SD_TIMEOUT;
}
int lume_sd_read_blocks(uintptr_t base,const struct lume_sd_card *card,
                        uint32_t block,uint32_t count,unsigned char *data){
    if(card->status!=LUME_SD_READY||block>=card->blocks||count>card->blocks-block||
       count>UINT32_MAX/512u||(!card->high_capacity&&
       (block>UINT32_MAX/512u||(count&&count-1u>UINT32_MAX/512u-block))))return LUME_SD_RESPONSE;
    if(!count)return LUME_SD_READY;
    if(!data||(uintptr_t)data>UINT32_MAX-count*512u)return LUME_SD_RESPONSE;
    if(count==1)return lume_sd_read(base,card,block,data);
    if(!block_capable(base)){
        for(uint32_t i=0;i<count;i++){
            int status=lume_sd_read(base,card,block+i,data+i*512u);
            io_clock_checkpoint();
            if(status!=LUME_SD_READY)return status;
        }
        return LUME_SD_READY;
    }
    int response=command(base,18,card->high_capacity?block:block*512u,1);
    if(response!=0){deselect(base);return LUME_SD_RESPONSE;}
    int status=LUME_SD_READY;
    for(uint32_t i=0;i<count;i++){
        status=receive(base,data+i*512u,512);io_clock_checkpoint();
        if(status!=LUME_SD_READY)break;
    }
    int stopped=stop_read(base);
    if(status==LUME_SD_READY)status=stopped;
    deselect(base);return status;
}
static void query_written(uintptr_t base,uint32_t requested){
    unsigned char data[4];
    if(command(base,55,0,1)!=0||command(base,22,0,1)!=0||receive(base,data,4)!=LUME_SD_READY)return;
    uint32_t count=((uint32_t)data[0]<<24)|((uint32_t)data[1]<<16)|((uint32_t)data[2]<<8)|data[3];
    if(count<=requested)lume_sd_partial_blocks=count;
}
static int verify_written(uintptr_t base,const struct lume_sd_card *card,uint32_t block,uint32_t count,const unsigned char *data){
    unsigned char check[512];
    if(count==1){
        int status=lume_sd_read(base,card,block,check);if(status!=LUME_SD_READY)return status;
        for(unsigned i=0;i<512;i++)if(check[i]!=data[i])return LUME_SD_CRC;
        return LUME_SD_READY;
    }
    if(command(base,18,card->high_capacity?block:block*512u,1)!=0){deselect(base);return LUME_SD_RESPONSE;}
    int status=LUME_SD_READY;
    for(uint32_t i=0;i<count;i++){
        status=receive(base,check,512);io_clock_checkpoint();if(status!=LUME_SD_READY)break;
        for(unsigned j=0;j<512;j++)if(check[j]!=data[i*512u+j]){status=LUME_SD_CRC;break;}
        if(status!=LUME_SD_READY)break;
    }
    int stopped=stop_read(base);deselect(base);
    return status==LUME_SD_READY?stopped:status;
}
int lume_sd_identify(uintptr_t base,struct lume_sd_card *card) {
    card->status=LUME_SD_ABSENT;card->blocks=card->capacity_mib=card->high_capacity=card->mbr_signature=0;
    if(!(lume_hal_read32(base+12u)&2u))return 0;
    card->status=LUME_SD_TRANSPORT;
    lume_hal_write32(base,125);lume_hal_write32(base+4u,1);
    for(unsigned i=0;i<10;++i)if(transfer(base,255)<0)goto done;
    card->status=LUME_SD_RESPONSE;
    if(command(base,0,0,0x95)!=1)goto done;
    int reply=command(base,8,0x1aa,0x87);
    unsigned v2=reply==1;
    if(v2) {
        unsigned char r7[4];
        for(unsigned i=0;i<4;++i) { int byte=transfer(base,255);if(byte<0)goto done;r7[i]=(unsigned char)byte; }
        if(r7[0]||r7[1]||r7[2]!=1||r7[3]!=0xaa)goto done;
    } else if(reply!=5)goto done; // CMD8 illegal on SD v1.
    card->status=LUME_SD_TIMEOUT;
    unsigned tries=1000;
    do {
        io_clock_checkpoint();
        reply=command(base,55,0,1);if(reply!=0&&reply!=1)goto done;
        reply=command(base,41,v2?0x40000000u:0,1);
        if(reply==0)break;if(reply!=1)goto done;
    } while(--tries);
    if(!tries)goto done;
    card->status=LUME_SD_RESPONSE;
    if(command(base,58,0,1)!=0)goto done;
    unsigned char ocr[4];
    for(unsigned i=0;i<4;++i) { int byte=transfer(base,255);if(byte<0)goto done;ocr[i]=(unsigned char)byte; }
    if(!(ocr[0]&128u)||!(ocr[1]&0x30u))goto done; // Card must accept the 3.3 V supply window.
    card->high_capacity=(unsigned)(v2&&(ocr[0]&64u));
    if(!card->high_capacity&&command(base,16,512,1)!=0)goto done;
    unsigned char csd[16];
    if(command(base,9,0,1)!=0)goto done;
    card->status=(uint32_t)receive(base,csd,16);
    if(card->status!=LUME_SD_READY)goto done;
    if(!lume_sd_decode_csd(csd,card)){card->status=LUME_SD_CSD;goto done;}
    card->status=LUME_SD_READY;
    /* Keep legacy3MHz; FIFO plus a compatible CSD permits30MHz/(2*3)=5MHz. */
    deselect(base);
    unsigned fast=block_capable(base)&&supports_spi5(csd);
    lume_hal_write32(base,fast?3u:5u);
    unsigned char sector[512];
    card->status=(uint32_t)lume_sd_read(base,card,0,sector);
    if(card->status==LUME_SD_READY)card->mbr_signature=sector[510]==0x55&&sector[511]==0xaa;
done:
    deselect(base);return card->status==LUME_SD_READY;
}
