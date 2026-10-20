/* queue_lock protects admission/commit; a separate per-process lease protects
 * generator work outside it. SHA checkpoints may suspend the private kernel
 * stack. No global PRNG state is modified until the owner commits it. */
static struct lume_random_state kernel_random;
static unsigned random_owner,random_wait_mask;
static uint32_t random_wait_pid[MAX_PROCESSES];
#include "crypto_aes.inc"
#include "crypto_curve.inc"
static void crypto_reset(unsigned slot){
    if(random_owner==slot+1u)panic("RNG owner teardown");
    random_wait_mask&=~(1u<<slot);random_wait_pid[slot]=0;
    if(aes_owner==slot+1u)panic("AES owner teardown");
    aes_wait_mask&=~(1u<<slot);aes_wait_pid[slot]=0;
    if(curve_owner==slot+1u)panic("Curve owner teardown");
    curve_wait_mask&=~(1u<<slot);curve_wait_pid[slot]=0;
}
void lume_random_checkpoint(void){lume_clock_checkpoint();}
static int random_acquire(unsigned slot){
    if(!random_owner){random_owner=slot+1u;return 1;}
    if(random_owner==slot+1u)panic("Recursive RNG owner");
    random_wait_mask|=1u<<slot;random_wait_pid[slot]=processes[slot].pid;
    processes[slot].state=LUME_WAITING;return 0;
}
static void random_release(unsigned slot){
    if(random_owner!=slot+1u)panic("RNG completion owner");random_owner=0;
    unsigned waiting=random_wait_mask;random_wait_mask=0;
    for(unsigned i=0;i<MAX_PROCESSES;i++)if(waiting&(1u<<i)){
        if(processes[i].pid!=random_wait_pid[i]||processes[i].state!=LUME_WAITING)continue;
        processes[i].context.regs[1]=LUME_CRYPTO;
        fs_pending[i]=(struct fs_pending){processes[i].pid,FS_RETRY_SYSCALL,0,0};
        processes[i].state=LUME_READY;lume_io_ready(i);
    }
}
static int crypto_service(unsigned slot,unsigned uid,unsigned op,uint32_t pointer,uint32_t bytes){
    if(bytes!=sizeof(struct lume_crypto_request)||op<LUME_CRYPTO_INFO||op>LUME_CRYPTO_ED25519_VERIFY_POINT)return -22;
    if((pointer&3u)||!user_range(slot,pointer,bytes,0))return -14;
    struct lume_crypto_request r;user_copy_from(slot,&r,pointer,sizeof r);
    if(r.version!=LUME_CRYPTO_VERSION||r.bytes!=sizeof r||r.flags)return -22;
    if(op>=LUME_CRYPTO_X25519)return curve_service(slot,op,&r);
    if(op>=LUME_CRYPTO_AES_ENCRYPT)return aes_service(slot,op,&r);
    if(op==LUME_CRYPTO_CLEAR){
        if(uid)return -13;if(r.buffer||r.length)return -22;
        if(!random_acquire(slot))return 0;
        lume_secret_clear(&kernel_random,sizeof kernel_random);random_release(slot);return 0;
    }
    if(op==LUME_CRYPTO_INFO){
        if(r.length!=sizeof(struct lume_crypto_info))return -22;
        if(!user_range(slot,r.buffer,r.length,1))return -14;
        struct lume_crypto_info info={LUME_CRYPTO_VERSION,sizeof info,
            kernel_random.ready&&kernel_random.remaining?LUME_CRYPTO_RANDOM_READY:0,
            LUME_CRYPTO_HMAC_DRBG_SHA256|(aes_device.available?LUME_CRYPTO_AES_GCM:0)|
                (curve_device.available?LUME_CRYPTO_X25519_HW|LUME_CRYPTO_ED25519_HW:0),
            (aes_device.available?LUME_CRYPTO_AES_GCM:0)|
                (curve_device.available?LUME_CRYPTO_X25519_HW|LUME_CRYPTO_ED25519_HW:0),LUME_RANDOM_MAX,LUME_RANDOM_SEED_BYTES,
            kernel_random.remaining,kernel_random.reseeds};
        user_copy_to(slot,r.buffer,&info,sizeof info);return 0;
    }
    if(op==LUME_CRYPTO_SEED){
        if(uid)return -13;
        if(r.length!=LUME_RANDOM_SEED_BYTES)return -22;
        if(!user_range(slot,r.buffer,r.length,0))return -14;
        if(!random_acquire(slot))return 0;
        unsigned char seed[LUME_RANDOM_SEED_BYTES];user_copy_from(slot,seed,r.buffer,sizeof seed);
        struct lume_random_state work=kernel_random;fs_release();unlock_kernel();
        int result=lume_random_seed(&work,seed,sizeof seed);lume_secret_clear(seed,sizeof seed);
        lock_kernel();kernel_random=work;lume_secret_clear(&work,sizeof work);random_release(slot);return result;
    }
    if(r.length>LUME_RANDOM_MAX)return -22;
    if(!user_range(slot,r.buffer,r.length,1))return -14;
    if(!random_acquire(slot))return 0;
    struct lume_random_state work=kernel_random;unsigned char buffer[LUME_RANDOM_MAX];fs_release();unlock_kernel();
    int result=lume_random_bytes(&work,buffer,r.length);
    lock_kernel();kernel_random=work;lume_secret_clear(&work,sizeof work);random_release(slot);
    if(result>0)user_copy_to(slot,r.buffer,buffer,(unsigned)result);
    lume_secret_clear(buffer,sizeof buffer);return result;
}
