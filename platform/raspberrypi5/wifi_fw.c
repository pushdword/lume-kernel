/* CYW43455 rev6 firmware, SDPCM/BCDC control and station Ethernet transport.
 * Linux brcmfmac register/wire-format references are recorded in WIRELESS.md. */
#include "lume/early.h"
#include "lume/sdio.h"
#include "lume/wifi.h"
#include "lume/wifi_assets.h"
#include "lume/vfs.h"
#include "lume/wireless.h"
#include "lume/wifi_protocol.h"
#include "lume/wpa.h"
#define RAM_BASE 0x198000u
static const struct lume_sdio_bus *bus;
static struct lume_wifi_cores cores;
static unsigned char transfer[512],verify[512],nvram[4096],raw_nvram[4096];
static unsigned char tx[2048],rx[4096],iov[1450],answer[1024];
static bool operational,configured,carrier,ssid_ok,psk_ok,joining,scan_running,scan_done;
static uint16_t scan_id;
static struct lume_access_point scan_results[LUME_SCAN_MAX];
static unsigned scan_count,scan_errors;
static unsigned char receive_queue[8][1514];static uint16_t receive_size[8];
static unsigned receive_head,receive_count,receive_drops;
static struct lume_wpa supplicant;static unsigned char station_mac[6];
static bool authentication_ready;
static unsigned char auth_queue[4][512];static uint16_t auth_size[4];
static unsigned auth_head,auth_count;
static void dispatch(const struct lume_wifi_frame *);
static unsigned char tx_sequence,tx_credit,rx_sequence;
static unsigned char flow_mask;static bool hardware_flow;
static bool frame_pending,rx_seen;
static uint16_t request_id;
static bool data_in_flight;static uint32_t data_sent_at;
static unsigned requests,responses,events;
static uint32_t stage,last_mailbox;
static uint32_t get32(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(unsigned char *p,uint32_t v)
{for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i));}
static bool read_reg(uint32_t a,uint32_t *v){return lume_sdio_backplane_read(bus,a,v);}
static bool write_reg(uint32_t a,uint32_t v){return lume_sdio_backplane_write(bus,a,v);}
static bool write_byte(unsigned fn,uint32_t a,unsigned char v)
{return lume_sdio_direct(bus,true,fn,a,&v);}
static bool read_byte(unsigned fn,uint32_t a,unsigned char *v)
{return lume_sdio_direct(bus,false,fn,a,v);}
static void mark(unsigned n){stage=n;lume_value("WIFI_FW_STAGE",stage);}
static bool wait_reg(uint32_t a,uint32_t mask,uint32_t expected)
{
    for(unsigned n=0;n<100;++n) {
        uint32_t v;if(!read_reg(a,&v))return false;
        if((v&mask)==expected)return true;
        bus->delay_us(100);
    }
    return false;
}
static bool disable_core(uint32_t wrap,unsigned pre,unsigned hold)
{
    uint32_t reset;
    if(!read_reg(wrap+0x800,&reset))return false;
    if(!(reset&1)) {
        if(!write_reg(wrap+0x408,pre|3) || !read_reg(wrap+0x408,&reset) ||
           !write_reg(wrap+0x800,1))return false;
        bus->delay_us(10);
        if(!wait_reg(wrap+0x800,1,1))return false;
    }
    return write_reg(wrap+0x408,hold|3) && read_reg(wrap+0x408,&reset);
}
static bool reset_core(uint32_t wrap,unsigned pre,unsigned hold)
{
    if(!disable_core(wrap,pre,hold))return false;
    for(unsigned n=0;n<100;++n) {
        uint32_t r;
        if(!write_reg(wrap+0x800,0))return false;
        bus->delay_us(40);
        if(!read_reg(wrap+0x800,&r))return false;
        if(!(r&1))return write_reg(wrap+0x408,hold|1) && read_reg(wrap+0x408,&r);
    }
    return false;
}
static int open_asset(struct lume_files *files,const char *path,size_t bytes,uint32_t crc)
{
    int64_t fd=lume_vfs_open(files,path,LUME_O_RDONLY);struct lume_stat info;
    if(fd<0 || lume_vfs_stat(files,(uint64_t)fd,&info) || info.size!=bytes)return -1;
    uint32_t actual=0;size_t done=0;
    while(done<bytes) {
        size_t count=bytes-done;if(count>sizeof(transfer))count=sizeof(transfer);
        if(lume_vfs_read(files,(uint64_t)fd,transfer,count)!=(int64_t)count)return -1;
        actual=lume_wifi_crc32(actual,transfer,count);done+=count;
    }
    if(actual!=crc || lume_vfs_seek(files,(uint64_t)fd,0,LUME_SEEK_SET))return -1;
    return (int)fd;
}
static bool file_read(struct lume_files *f,int fd,size_t offset,void *out,size_t bytes)
{
    return lume_vfs_seek(f,(unsigned)fd,(int64_t)offset,LUME_SEEK_SET)==(int64_t)offset &&
           lume_vfs_read(f,(unsigned)fd,out,bytes)==(int64_t)bytes;
}
static bool write_verify(uint32_t address,void *data,size_t bytes)
{
    if(bytes>sizeof(verify) || !lume_sdio_memory(bus,true,address,data,bytes) ||
       !lume_sdio_memory(bus,false,address,verify,bytes))return false;
    const unsigned char *p=data;
    for(size_t i=0;i<bytes;++i)if(p[i]!=verify[i]) {
        lume_value("WIFI_FW_VERIFY_ADDRESS",address+i);
        lume_value("WIFI_FW_VERIFY_EXPECTED",p[i]);lume_value("WIFI_FW_VERIFY_ACTUAL",verify[i]);
        return false;
    }
    return true;
}
static bool upload(struct lume_files *f,int fd,size_t bytes,uint32_t *reset_vector)
{
    uint32_t crc=0;
    for(size_t offset=0;offset<bytes;) {
        size_t count=bytes-offset;if(count>sizeof(transfer))count=sizeof(transfer);
        if(!file_read(f,fd,offset,transfer,count))return false;
        if(!offset)*reset_vector=get32(transfer);
        size_t padded=(count+3)&~(size_t)3;
        for(size_t n=count;n<padded;++n)transfer[n]=0;
        if(!write_verify(RAM_BASE+(uint32_t)offset,transfer,padded)) {
            lume_value("WIFI_FW_UPLOAD_ERROR_OFFSET",offset);return false;
        }
        crc=lume_wifi_crc32(crc,verify,count);offset+=count;
    }
    lume_value("WIFI_FW_READBACK_CRC32",crc);
    return crc==LUME_WIFI_FIRMWARE_CRC32;
}
static bool fifo(bool write,unsigned char *p,size_t bytes)
{
    if(!bytes || (bytes&3))return false;
    while(bytes) {
        size_t count=bytes>2048?2048:bytes;
        bool blocks=count>=512;
        if(blocks)count&=~(size_t)511;
        uint32_t arg=(write?0x80000000u:0)|(2u<<28)|(blocks?1u<<27:0)|
                     (uint32_t)(blocks?count/512:count&511);
        if(write?!bus->write53(arg,p,count):!bus->read53(arg,p,count))return false;
        p+=count;bytes-=count;
    }
    return true;
}
static int packet(void)
{
    uint32_t ints;
    if(!read_reg(cores.sdio+0x20,&ints))return -1;
    hardware_flow=(ints&0x10)!=0;
    if((ints&0xe0) && !write_reg(cores.sdio+0x20,ints&0xe0))return -1;
    if(ints&0x20) {
        uint32_t fresh;if(!read_reg(cores.sdio+0x20,&fresh))return -1;
        hardware_flow=(fresh&0x30)!=0;ints|=fresh;
    }
    if(ints&0x80) {
        if(!read_reg(cores.sdio+0x4c,&last_mailbox) || !write_reg(cores.sdio+0x40,2))return -1;
        if(last_mailbox&0x10){lume_value("WIFI_FW_HALTED",last_mailbox);return -1;}
    }
    if(ints&0x40)frame_pending=true;
    if(!frame_pending)return 0;
    if(!fifo(false,rx,12))return -1;
    if(!(rx[0]|rx[1])){frame_pending=false;return 0;}
    struct lume_wifi_frame h;
    if(!lume_wifi_frame_header(rx,12,&h)) {
        lume_value("WIFI_FW_BAD_SDPCM_HEADER",get32(rx));return -1;
    }
    if(h.length>12 && !fifo(false,rx+12,(h.length-12+3)&~(size_t)3))return -1;
    if(rx_seen && h.sequence!=(unsigned char)(rx_sequence+1)) {
        lume_value("WIFI_FW_RX_SEQUENCE",h.sequence);lume_value("WIFI_FW_RX_PREVIOUS",rx_sequence);return -1;
    }
    rx_seen=true;rx_sequence=h.sequence;tx_credit=h.credit;flow_mask=h.flow;
    if(h.channel!=0)dispatch(&h);
    return (int)h.length;
}
static bool wait_transmit(void)
{
    for(unsigned n=0;n<2000;++n) {
        /* Refresh both hardware XOFF and the SDPCM window before every TX. */
        if(packet()<0)return false;
        if(lume_wifi_tx_allowed(tx_sequence,tx_credit,flow_mask,hardware_flow))return true;
        if(n%100==99 && !write_reg(cores.sdio+0x40,8))return false;
        bus->delay_us(1000);
    }
    lume_value("WIFI_TX_STALL_FLOW",flow_mask);lume_value("WIFI_TX_STALL_HARDWARE",hardware_flow);
    return false;
}
static void settle_data(void)
{
    if(!data_in_flight)return;
    /* PIO completion precedes firmware/air completion of EAPOL. In particular
     * do not install PTK while firmware still owns the unencrypted M4. This
     * polling port has no per-frame firmware TX-status completion yet. */
    uint32_t elapsed=lume_clock_millis()-data_sent_at;
    if(elapsed<50)bus->delay_us((50-elapsed)*1000);
    data_in_flight=false;
}
static int control(uint32_t op,bool set,const void *input,size_t bytes,void *output,size_t cap,size_t *got)
{
    settle_data();
    if(request_id==UINT16_MAX)return -1;
    uint16_t id=++request_id;
    if(!wait_transmit())return -1;
    size_t count=lume_wifi_control_request(tx,sizeof(tx),tx_sequence,id,op,set,input,bytes,set?0:cap);
    if(!count)return -1;
    if(!fifo(true,tx,count))return -1;
    ++tx_sequence;++requests;
    for(unsigned n=0;n<3000;++n) {
        int length=packet();if(length<0)return -1;
        if(length) {
            int32_t status;
            int result=lume_wifi_control_reply(rx,(size_t)length,id,op,output,cap,got,&status);
            if(result==1){++responses;return 1;}
            if(result==-2){++responses;lume_value("WIFI_FW_CONTROL_STATUS",(uint32_t)status);return -2;}
            if(result<0) {
                struct lume_wifi_frame h;
                if(lume_wifi_frame_header(rx,(size_t)length,&h) && h.length-h.offset>=16) {
                    lume_value("WIFI_FW_BAD_CDC_LENGTH",get32(rx+h.offset+4));
                    lume_value("WIFI_FW_BAD_CDC_FLAGS",get32(rx+h.offset+8));
                }
                return -1;
            }
            ++events;
        }
        bus->delay_us(1000);
    }
    lume_value("WIFI_FW_TIMEOUT_COMMAND",op);return -1;
}
static int get_variable(const char *name,void *out,size_t cap,size_t *got)
{
    size_t bytes=0;while(name[bytes])++bytes;
    size_t wire=cap>bytes+1?cap:bytes+1,actual=0;
    if(wire>sizeof(iov) || !out || !got)return -1;
    /* GET_VAR shares its request/response buffer. The name can be longer
     * than the returned value (e.g. clmload_status is a 32-bit integer). */
    int result=control(262,false,name,bytes+1,iov,wire,&actual);
    *got=0;
    if(result==1) {
        if(actual>cap)actual=cap;
        unsigned char *p=out;for(size_t n=0;n<actual;++n)p[n]=iov[n];
        *got=actual;
    }
    return result;
}
static void print_string(const char *label,const unsigned char *s,size_t count)
{
    lume_puts(label);lume_putc('=');
    for(size_t n=0;n<count && s[n];++n)lume_putc(s[n]>=32 && s[n]<127?(char)s[n]:' ');
    lume_putc('\n');
}
static bool run(struct lume_files *files)
{
    mark(1);
    int fw=open_asset(files,"/lib/firmware/wifi.bin",LUME_WIFI_FIRMWARE_BYTES,LUME_WIFI_FIRMWARE_CRC32);
    int nv=open_asset(files,"/lib/firmware/wifi.txt",LUME_WIFI_NVRAM_BYTES,LUME_WIFI_NVRAM_CRC32);
    int clm=open_asset(files,"/lib/firmware/wifi.clm",LUME_WIFI_CLM_BYTES,LUME_WIFI_CLM_CRC32);
    if(fw<0 || nv<0 || clm<0 || !file_read(files,nv,0,raw_nvram,LUME_WIFI_NVRAM_BYTES))return false;
    size_t nvbytes=lume_wifi_nvram(raw_nvram,LUME_WIFI_NVRAM_BYTES,nvram,sizeof(nvram));
    if(!nvbytes)return false;
    lume_value("WIFI_NVRAM_PACKED_BYTES",nvbytes);
    mark(2);uint32_t erom;
    if(!lume_wifi_transfer_clock() || !read_reg(0x180000fc,&erom))return false;
    lume_value("WIFI_EROM",erom);
    if(erom<0x18000000 || erom>0x181ffe00 || (erom&3))return false;
    if(!lume_sdio_memory(bus,false,erom,transfer,512))return false;
    if(!lume_wifi_erom(transfer,512,&cores)) {
        for(unsigned n=0;n<512;n+=4)lume_value("WIFI_EROM_WORD",get32(transfer+n));
        return false;
    }
    lume_value("WIFI_ARM_REGS",cores.arm);
    lume_value("WIFI_ARM_WRAP",cores.arm_wrap);lume_value("WIFI_SDIO_REGS",cores.sdio);
    lume_value("WIFI_D11_WRAP",cores.d11_wrap);
    mark(3);
    if(!reset_core(cores.arm_wrap,0x20,0x20) || !disable_core(cores.d11_wrap,12,4))return false;
    uint32_t cap;
    if(!read_reg(cores.arm+4,&cap))return false;
    unsigned banks=(cap&15)+((cap>>4)&15);if(!banks || banks>16)return false;
    cores.ram_bytes=0;
    for(unsigned n=0;n<banks;++n) {
        uint32_t info;
        if(!write_reg(cores.arm+0x40,n) || !read_reg(cores.arm+0x44,&info))return false;
        cores.ram_bytes+=((info&127)+1)*((info&0x200)?1024u:8192u);
    }
    lume_value("WIFI_RAM_BASE",RAM_BASE);lume_value("WIFI_RAM_BYTES",cores.ram_bytes);
    if(cores.ram_bytes>2097152 || cores.ram_bytes<65536 ||
       ((LUME_WIFI_FIRMWARE_BYTES+3)&~3u)>cores.ram_bytes-nvbytes-4)return false;
    if(!write_byte(1,0x1000f,0) || !write_reg(cores.common+0x58,0) || !write_reg(cores.common+0x5c,0))return false;
    mark(4);uint32_t vector=0,top=RAM_BASE+cores.ram_bytes;
    if(!write_reg(top-4,0) || !upload(files,fw,LUME_WIFI_FIRMWARE_BYTES,&vector))return false;
    for(size_t offset=0;offset<nvbytes;) {
        size_t count=nvbytes-offset;if(count>512)count=512;
        if(!write_verify(top-(uint32_t)nvbytes-4+(uint32_t)offset,nvram+offset,count))return false;
        offset+=count;
    }
    uint32_t words=(uint32_t)nvbytes/4,token=words|((~words&0xffff)<<16),check;
    if(!write_reg(top-4,token) || !read_reg(top-4,&check) || check!=token)return false;
    lume_value("WIFI_FW_BYTES_VERIFIED",LUME_WIFI_FIRMWARE_BYTES);lume_value("WIFI_NVRAM_TOKEN",token);
    mark(5);
    if(!vector || !write_reg(cores.sdio+0x20,UINT32_MAX) || !write_reg(0,vector) ||
       !reset_core(cores.arm_wrap,0x20,0))return false;
    if(!write_byte(1,0x1000e,0))return false;
    bus->delay_us(1000);
    if(!write_byte(1,0x1000e,0x10))return false;
    bool ready=false;unsigned char csr;
    for(unsigned n=0;n<2000;++n) {
        if(!read_byte(1,0x1000e,&csr))return false;
        if(csr&0x80){ready=true;break;}bus->delay_us(1000);
    }
    if(!ready || !write_byte(1,0x1000e,(unsigned char)(csr|2)) ||
       !write_byte(0,0x210,0) || !write_byte(0,0x211,2) ||
       !write_reg(cores.sdio+0x48,4u<<16) || !write_reg(cores.sdio+0x24,0xe0) ||
       !write_byte(0,2,6))return false;
    ready=false;
    for(unsigned n=0;n<2000;++n) {
        unsigned char v;if(!read_byte(0,3,&v))return false;
        if((v&6)==6){ready=true;break;}bus->delay_us(1000);
    }
    if(!ready || !write_byte(0,4,7))return false;
    mark(6);ready=false;
    for(unsigned n=0;n<3000;++n) {
        if(packet()<0)return false;
        if(last_mailbox&8){ready=true;break;}bus->delay_us(1000);
    }
    lume_value("WIFI_FW_MAILBOX",last_mailbox);
    if(!ready || ((last_mailbox>>16)&255)!=4)return false;
    lume_puts("LUME CHECK WIFI FIRMWARE BOOT PASS\n");
    mark(7);size_t got;
    if(control(0,false,0,0,answer,4,&got)!=1 || got!=4 || get32(answer)!=0x14e46c77)return false;
    lume_value("WIFI_FW_IOCTL_MAGIC",get32(answer));
    if(control(1,false,0,0,answer,4,&got)!=1 || got!=4 || get32(answer)!=2)return false;
    lume_value("WIFI_FW_IOCTL_VERSION",get32(answer));
    if(get_variable("ver",answer,256,&got)!=1 || !got)return false;
    print_string("WIFI_FW_VERSION",answer,got);
    mark(8);
    if(get_variable("lume_bad_iovar",answer,4,&got)!=-2)return false;
    if(get_variable("ver",answer,256,&got)!=1)return false;
    lume_puts("LUME CHECK WIFI CONTROL ERROR RECOVERY PASS\n");
    mark(9);
    for(size_t offset=0;offset<LUME_WIFI_CLM_BYTES;) {
        size_t n=LUME_WIFI_CLM_BYTES-offset;if(n>1000)n=1000;
        size_t padded=(n+7)&~(size_t)7;
        for(size_t k=0;k<sizeof(iov);++k)iov[k]=0;
        const char name[]="clmload";for(unsigned k=0;k<sizeof(name);++k)iov[k]=(unsigned char)name[k];
        unsigned flags=0x1000|(offset?0:2)|(offset+n==LUME_WIFI_CLM_BYTES?4:0);
        iov[8]=(unsigned char)flags;iov[9]=(unsigned char)(flags>>8);iov[10]=2;
        put32(iov+12,(uint32_t)padded);
        if(!file_read(files,clm,offset,iov+20,n) || control(263,true,iov,20+padded,answer,sizeof(answer),&got)!=1)return false;
        offset+=n;
    }
    if(get_variable("clmload_status",answer,4,&got)!=1 || got!=4 || get32(answer))return false;
    lume_value("WIFI_CLM_STATUS",get32(answer));
    if(get_variable("clmver",answer,512,&got)!=1 || !got)return false;
    print_string("WIFI_CLM_VERSION",answer,got);
    lume_puts("LUME CHECK WIFI CLM LOAD PASS\n");mark(10);
    lume_puts("LUME CHECK WIFI FIRMWARE CONTROL PASS\n");return true;
}
bool lume_wifi_firmware(void)
{
    bus=lume_wifi_bus();if(!bus || !bus->write53)return false;
    cores=(struct lume_wifi_cores){0};tx_sequence=rx_sequence=0;tx_credit=2;
    request_id=0;frame_pending=rx_seen=false;requests=responses=events=stage=last_mailbox=0;
    flow_mask=0;hardware_flow=false;
    data_in_flight=false;
    struct lume_files files;lume_files_init(&files);
    bool ok=run(&files);lume_files_release(&files);
    if(!ok)lume_value("WIFI_FW_FAILED_STAGE",stage);
    lume_value("WIFI_FW_CONTROL_REQUESTS",requests);lume_value("WIFI_FW_CONTROL_RESPONSES",responses);
    lume_value("WIFI_FW_EVENT_FRAMES",events);return ok;
}
static bool set_variable(const char *name,const void *data,size_t bytes)
{
    size_t n=0,got;while(name[n])++n;++n;
    if(n+bytes>sizeof(iov))return false;
    for(size_t i=0;i<n;++i)iov[i]=(unsigned char)name[i];
    const unsigned char *p=data;for(size_t i=0;i<bytes;++i)iov[n+i]=p[i];
    int result=control(263,true,iov,n+bytes,answer,sizeof(answer),&got);
    if(result!=1){lume_puts("WIFI_SET_FAILED=");lume_puts(name);lume_putc('\n');}
    /* Secret supplicant material never survives in the transport buffer. */
    volatile unsigned char *wipe=iov;for(size_t i=0;i<n+bytes;++i)wipe[i]=0;
    wipe=tx;for(size_t i=0;i<sizeof(tx);++i)wipe[i]=0;
    return result==1;
}
static bool set_u32(const char *name,uint32_t v)
{unsigned char p[4];put32(p,v);return set_variable(name,p,4);}
static bool command_u32(unsigned op,uint32_t v)
{unsigned char p[4];size_t got;put32(p,v);return control(op,true,p,4,answer,sizeof(answer),&got)==1;}
static void dispatch(const struct lume_wifi_frame *h)
{
    if(h->channel!=1 && h->channel!=2)return;
    if(h->length-h->offset<4)return;
    const unsigned char *p=rx+h->offset;size_t skip=4+(size_t)p[3]*4;
    if((p[0]>>4)!=2 || (p[2]&15) || skip>h->length-h->offset)return;
    size_t n=h->length-h->offset-skip;p+=skip;
    if(h->channel==2) {
        if(authentication_ready && n>=14 && p[12]==0x88 && p[13]==0x8e) {
            if(n>512 || auth_count==4){++receive_drops;return;}
            unsigned slot=(auth_head+auth_count)%4;for(size_t i=0;i<n;++i)auth_queue[slot][i]=p[i];
            auth_size[slot]=(uint16_t)n;++auth_count;return;
        }
        if(!operational || n<14 || n>1514 || receive_count==8){++receive_drops;return;}
        unsigned slot=(receive_head+receive_count)%8;
        for(size_t i=0;i<n;++i)receive_queue[slot][i]=p[i];receive_size[slot]=(uint16_t)n;++receive_count;return;
    }
    struct lume_wifi_event e;if(!lume_wifi_event_parse(p,n,&e))return;
    ++events;
    if(e.type==69) {
        if(!scan_running)return;
        if(e.status==8) {
            if(e.bytes<12 || get32(e.data)>e.bytes || get32(e.data)<12 ||
               (e.data[8]|(unsigned)e.data[9]<<8)!=scan_id){++scan_errors;return;}
            size_t pos=12,limit=get32(e.data);unsigned count=e.data[10]|(unsigned)e.data[11]<<8;
            if(count>32){++scan_errors;return;}
            for(unsigned i=0;i<count;++i) {
                struct lume_access_point ap;size_t used;
                if(!lume_wifi_bss(e.data+pos,limit-pos,&ap,&used)){++scan_errors;break;}
                unsigned slot=0;for(;slot<scan_count;++slot) {
                    unsigned diff=0;for(unsigned k=0;k<6;++k)diff|=scan_results[slot].bssid[k]^ap.bssid[k];
                    if(!diff)break;
                }
                if(slot<LUME_SCAN_MAX){scan_results[slot]=ap;if(slot==scan_count)++scan_count;}
                pos+=used;
            }
        } else {scan_done=true;if(e.status)++scan_errors;}
        return;
    }
    lume_value("WIFI_EVENT_TYPE",e.type);lume_value("WIFI_EVENT_STATUS",e.status);
    lume_value("WIFI_EVENT_REASON",e.reason);
    lume_value("WIFI_EVENT_FLAGS",e.flags);
    if(e.type==0) {ssid_ok=e.status==0;if(e.status)joining=false;}
    if(e.type==16){carrier=(e.flags&1)!=0;if(!carrier)ssid_ok=psk_ok=false;}
    if(e.type==46){psk_ok=e.status==6;if(e.status==7)joining=false;}
    if(e.type==6 || e.type==12){carrier=ssid_ok=psk_ok=false;joining=false;}
}
bool lume_wifi_prepare(const unsigned char mac[6])
{
    if(!mac || (mac[0]&1))return false;
    operational=carrier=ssid_ok=psk_ok=joining=scan_running=scan_done=false;
    receive_head=receive_count=receive_drops=0;
    authentication_ready=false;auth_head=auth_count=0;
    for(unsigned i=0;i<6;++i)station_mac[i]=mac[i];
    unsigned char country[12]={'P','T',0,0,255,255,255,255,'P','T',0,0},mask[16]={0};
    const unsigned ev[]={0,1,2,3,4,5,6,7,8,9,10,11,12,16,18,19,25,46,69};for(unsigned i=0;i<sizeof(ev)/sizeof(ev[0]);++i)mask[ev[i]/8]|=(unsigned char)(1u<<(ev[i]%8));
    size_t got;
    if(!set_variable("cur_etheraddr",mac,6) || get_variable("cur_etheraddr",answer,6,&got)!=1 || got!=6)return false;
    for(unsigned i=0;i<6;++i)if(answer[i]!=mac[i])return false;
    if(!set_variable("country",country,12) || !set_u32("bus:txglom",0) || !set_u32("bus:rxglom",0) || !set_u32("roam_off",1) ||
       !set_variable("event_msgs",mask,sizeof(mask)) || !command_u32(20,1) ||
       control(2,true,0,0,answer,sizeof(answer),&got)!=1 || !command_u32(86,0))return false;
    unsigned char entropy[32];if(!lume_entropy(entropy,sizeof(entropy)))return false;
    lume_secret_clear(entropy,sizeof(entropy));lume_puts("LUME CHECK HARDWARE ENTROPY PASS\n");
    bus->delay_us(50000);operational=configured=true;
    lume_puts("LUME CHECK WIFI HARDWARE MAC COUNTRY PT PASS\n");return true;
}
static bool install_key(const unsigned char *key,unsigned index,bool group,const unsigned char rsc[8])
{
    unsigned char wire[164]={0};put32(wire,index);put32(wire+4,16);put32(wire+112,4);put32(wire+116,group?2:0);
    for(unsigned i=0;i<16;++i)wire[8+i]=key[i];
    if(group){put32(wire+132,1);put32(wire+140,get32(rsc+2));wire[144]=rsc[0];wire[145]=rsc[1];}
    else for(unsigned i=0;i<6;++i)wire[156+i]=supplicant.peer[i];
    bool ok=set_variable("wsec_key",wire,sizeof(wire));lume_secret_clear(wire,sizeof(wire));return ok;
}
static bool send_auth(const void *data,size_t bytes)
{
    if(!wait_transmit())return false;
    size_t count=lume_wifi_data_frame(tx,sizeof(tx),tx_sequence,data,bytes);
    if(!count || !fifo(true,tx,count))return false;++tx_sequence;
    data_in_flight=true;data_sent_at=lume_clock_millis();
    return true;
}
static bool association_rsn(void)
{
    size_t got;
    if(get_variable("assoc_info",answer,64,&got)!=1 || got<4)return false;
    size_t count=get32(answer);
    if(!count || count>sizeof(answer) || get_variable("assoc_req_ies",answer,count,&got)!=1 || got<count)return false;
    for(size_t pos=0;pos+2<=count;) {
        size_t n=(size_t)answer[pos+1]+2;if(n>count-pos)return false;
        if(answer[pos]==48) {
            bool same=n==sizeof(lume_wpa_rsn);
            for(size_t j=0;same && j<n;++j)same=answer[pos+j]==lume_wpa_rsn[j];
            lume_value("WIFI_ASSOC_RSN_MATCH",same);lume_value("WIFI_ASSOC_RSN_BYTES",n);
            return lume_wpa_set_rsn(&supplicant,answer+pos,n);
        }
        pos+=n;
    }
    return false;
}
static void authenticate(void)
{
    if(!authentication_ready)return;
    for(unsigned n=0;n<4 && auth_count;++n) {
        struct lume_wpa_action action;unsigned char frame[512];size_t bytes=auth_size[auth_head];
        for(size_t i=0;i<bytes;++i)frame[i]=auth_queue[auth_head][i];
        auth_head=(auth_head+1)%4;--auth_count;
        if(!supplicant.seen && !association_rsn()) {joining=false;lume_secret_clear(frame,sizeof(frame));lume_puts("WIFI UNSUPPORTED ASSOCIATION RSN\n");break;}
        int result=lume_wpa_input(&supplicant,frame,bytes,&action);lume_secret_clear(frame,sizeof(frame));
        if(result<0){lume_puts("WIFI WPA2 INVALID EAPOL DROPPED\n");continue;}
        if(!result)continue;
        /* M4 must leave before installing PTK, otherwise firmware encrypts
         * the handshake completion with the key that it is acknowledging. */
        bool ok=send_auth(action.frame,action.bytes) &&
                (!action.pairwise || install_key(action.tk,0,false,action.rsc)) &&
                (!action.group || install_key(action.gtk,action.gtk_id,true,action.rsc));
        if(ok && action.authorize) {
            size_t got;ok=control(121,true,supplicant.peer,6,answer,sizeof(answer),&got)==1;
            if(ok){psk_ok=true;lume_puts("LUME CHECK WPA2 HOST HANDSHAKE KEYS PASS\n");}
        }
        lume_secret_clear(&action,sizeof(action));
        if(!ok){psk_ok=joining=false;lume_puts("WIFI WPA2 KEY INSTALL OR TRANSMIT FAILED\n");break;}
    }
}
void lume_wifi_poll(void)
{
    if(!operational)return;
    for(unsigned n=0;n<4;++n) {int r=packet();if(r<0){operational=carrier=false;lume_puts("WIFI TRANSPORT FAILED\n");break;}if(!r)break;}
    if(operational)authenticate();
}
int lume_wifi_receive(void *out,size_t cap)
{
    if(!receive_count)return 0;
    size_t bytes=receive_size[receive_head];
    if(bytes>cap)return -1;
    unsigned char *p=out;for(size_t i=0;i<bytes;++i)p[i]=receive_queue[receive_head][i];
    receive_head=(receive_head+1)%8;--receive_count;return (int)bytes;
}
bool lume_wifi_send(const void *data,size_t bytes)
{
    if(!operational || !lume_wifi_link() || packet()<0 ||
       !lume_wifi_tx_allowed(tx_sequence,tx_credit,flow_mask,hardware_flow))return false;
    size_t count=lume_wifi_data_frame(tx,sizeof(tx),tx_sequence,data,bytes);
    if(!count || !fifo(true,tx,count))return false;
    ++tx_sequence;data_in_flight=true;data_sent_at=lume_clock_millis();return true;
}
bool lume_wifi_link(void){return operational && carrier && ssid_ok && psk_ok;}
static bool start_interface(void)
{
    if(operational)return true;if(!configured)return false;
    /* This stepping and MAC were validated on the initial boot. A stopped
     * station restarts with a clean firmware/SDPCM session, not stale keys. */
    uint64_t frequency;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));
    return lume_wifi_probe(frequency,true) && lume_wifi_firmware() && lume_wifi_prepare(station_mac);
}
int lume_wifi_scan(struct lume_access_point *out,size_t capacity)
{
    if(!start_interface() || scan_running)return -1;
    unsigned char params[72]={0};put32(params,1);params[4]=1;
    if(!++scan_id)++scan_id;params[6]=(unsigned char)scan_id;params[7]=(unsigned char)(scan_id>>8);
    for(unsigned i=44;i<50;++i)params[i]=255;
    params[50]=2;for(unsigned i=52;i<68;++i)params[i]=255;
    scan_count=scan_errors=0;scan_running=true;scan_done=false;
    bool ok=set_variable("escan",params,sizeof(params));uint32_t start=lume_clock_millis();
    while(ok && !scan_done && (uint32_t)(lume_clock_millis()-start)<30000) {lume_wifi_poll();bus->delay_us(1000);}
    scan_running=false;
    lume_value("WIFI_SCAN_RESULTS",scan_count);lume_value("WIFI_SCAN_PARSE_ERRORS",scan_errors);
    if(!ok || !scan_done || scan_errors)return -1;
    unsigned count=scan_count;if(count>capacity)count=(unsigned)capacity;
    for(unsigned i=0;i<count;++i)out[i]=scan_results[i];return (int)count;
}
void lume_wifi_leave(void)
{
    /* The installed firmware does not reliably acknowledge DOWN/DISASSOC
     * after traffic. WL_ON provides a deterministic, bounded radio stop. */
    if(operational && (carrier || authentication_ready)) {settle_data();lume_wifi_stop();operational=false;}
    carrier=ssid_ok=psk_ok=joining=false;receive_count=0;authentication_ready=false;auth_count=0;lume_secret_clear(&supplicant,sizeof(supplicant));
}
bool lume_wifi_join(const unsigned char *ssid,size_t sn,const unsigned char *key,size_t kn)
{
    if(!configured || !ssid || !sn || sn>32 || !key || kn<8 || kn>63)return false;
    lume_wifi_leave();if(!start_interface())return false;joining=true;
    unsigned char network[36]={0};size_t got;
    bool ok=lume_wpa_begin(&supplicant,station_mac,ssid,sn,key,kn,lume_entropy);
    authentication_ready=ok;
    ok=ok && command_u32(20,1) && command_u32(22,0) && command_u32(134,4) &&
        set_u32("mfp",0) && set_variable("wpaie",lume_wpa_rsn,sizeof(lume_wpa_rsn)) && command_u32(165,0x80);
    put32(network,(uint32_t)sn);for(size_t i=0;i<sn;++i)network[4+i]=ssid[i];
    ok=ok && control(26,true,network,sizeof(network),answer,sizeof(answer),&got)==1;
    uint32_t start=lume_clock_millis();
    while(ok && joining && !lume_wifi_link() && (uint32_t)(lume_clock_millis()-start)<30000) {lume_wifi_poll();bus->delay_us(1000);}
    joining=false;
    if(!ok || !lume_wifi_link()){lume_wifi_leave();return false;}
    lume_puts("LUME CHECK WIFI WPA2 ASSOCIATION PASS\n");return true;
}

/* Bind this backend to the two common APIs; no lwIP types cross this boundary. */
static void cyw_poll(void *ctx){(void)ctx;lume_wifi_poll();}
static bool cyw_link(void *ctx){(void)ctx;return lume_wifi_link();}
static int cyw_receive(void *ctx,void *p,size_t n){(void)ctx;return lume_wifi_receive(p,n);}
static bool cyw_transmit(void *ctx,const void *p,size_t n){(void)ctx;return lume_wifi_send(p,n);}
static bool cyw_admin(void *ctx,bool up)
{
    (void)ctx;if(!up){lume_wifi_leave();if(operational){lume_wifi_stop();operational=false;}}return true;
}
static int cyw_scan(void *ctx,struct lume_wifi_bss_info *out,unsigned cap)
{
    (void)ctx;struct lume_access_point aps[LUME_SCAN_MAX];int n=lume_wifi_scan(aps,cap);
    if(n<0)return n;
    for(int i=0;i<n;++i) {
        out[i]=(struct lume_wifi_bss_info){0};
        for(unsigned k=0;k<6;++k)out[i].bssid[k]=aps[i].bssid[k];
        out[i].channel=aps[i].channel;out[i].rssi=aps[i].rssi;out[i].security=aps[i].security;out[i].ssid_bytes=aps[i].ssid_len;
        for(unsigned k=0;k<aps[i].ssid_len;++k)out[i].ssid[k]=(unsigned char)aps[i].ssid[k];
    }
    return n;
}
static bool cyw_connect(void *ctx,const unsigned char *s,size_t sn,const unsigned char *k,size_t kn)
{(void)ctx;return lume_wifi_join(s,sn,k,kn);}
static void cyw_disconnect(void *ctx){(void)ctx;lume_wifi_leave();}
static void cyw_peer(void *ctx,unsigned char out[6])
{(void)ctx;for(unsigned i=0;i<6;++i)out[i]=lume_wifi_link()?supplicant.peer[i]:0;}
bool lume_pi5_wifi_register(const unsigned char mac[6])
{
    static const struct lume_netif_ops data={cyw_poll,cyw_link,cyw_receive,cyw_transmit,cyw_admin};
    static const struct lume_wireless_ops control={cyw_scan,cyw_connect,cyw_disconnect,cyw_peer};
    const struct lume_wifi_caps caps={LUME_WIFI_STATION|LUME_WIFI_SCAN|LUME_WIFI_WPA2_CCMP,
        LUME_WIFI_BAND_2GHZ|LUME_WIFI_BAND_5GHZ,32,32,{'P','T'},{0}};
    if(!lume_wifi_prepare(mac))return false;
    struct lume_netif_descriptor descriptor={.name="wlan0",.kind=LUME_IF_WIFI,.mtu=1500,.ops=&data};
    for(unsigned i=0;i<6;++i)descriptor.mac[i]=mac[i];
    int index=lume_netif_register(&descriptor);
    if(index<0 || lume_wireless_register((unsigned)index,&caps,&control,0)<0 || lume_netif_admin(lume_netif_find((unsigned)index),true)<0)return false;
    lume_puts("LUME CHECK GENERIC NETIF WIFI API REGISTER PASS\n");return true;
}
