/* Complete-curve service admission under queue_lock, hardware work outside it.
 * A per-slot/PID lease survives kernel safe-point preemption. */
static struct lume_crypto_device curve_device;
static unsigned curve_owner,curve_wait_mask;
static uint32_t curve_wait_pid[MAX_PROCESSES];
static int curve_acquire(unsigned slot){
    if(!curve_owner){curve_owner=slot+1u;return 1;}
    if(curve_owner==slot+1u)panic("Recursive curve owner");
    curve_wait_mask|=1u<<slot;curve_wait_pid[slot]=processes[slot].pid;
    processes[slot].state=LUME_WAITING;return 0;
}
static void curve_release(unsigned slot){
    if(curve_owner!=slot+1u)panic("Curve completion owner");curve_owner=0;
    unsigned waiting=curve_wait_mask;curve_wait_mask=0;
    for(unsigned i=0;i<MAX_PROCESSES;i++)if(waiting&(1u<<i)){
        if(processes[i].pid!=curve_wait_pid[i]||processes[i].state!=LUME_WAITING)continue;
        processes[i].context.regs[1]=LUME_CRYPTO;
        fs_pending[i]=(struct fs_pending){processes[i].pid,FS_RETRY_SYSCALL,0,0};
        processes[i].state=LUME_READY;lume_io_ready(i);
    }
}
static int curve_service(unsigned slot,unsigned op,const struct lume_crypto_request *request){
    if(request->length!=sizeof(struct lume_curve_request))return -22;
    if((request->buffer&3u)||!user_range(slot,request->buffer,request->length,0))return -14;
    struct lume_curve_request r;user_copy_from(slot,&r,request->buffer,sizeof r);
    if(r.version!=1||r.bytes!=sizeof r)return -22;
    unsigned operation=op-LUME_CRYPTO_X25519+1u;
    if((operation!=3&&r.scalar_b)||(operation==2&&r.point))return -22;
    if(!user_range(slot,r.scalar_a,32,0)||!user_range(slot,r.output,32,1)||
       (operation==3&&!user_range(slot,r.scalar_b,32,0))||
       (operation!=2&&!user_range(slot,r.point,32,0))||
       (r.cycles&&((r.cycles&3u)||!user_range(slot,r.cycles,4,1))))return -14;
    if(r.cycles&&r.output<r.cycles+4u&&r.cycles<r.output+32u)return -22;
    if(!curve_device.available)return -95;
    if(!curve_acquire(slot))return 0;
    struct lume_curve_packet p={.operation=operation};
    user_copy_from(slot,p.scalar_a,r.scalar_a,32);
    if(operation==3)user_copy_from(slot,p.scalar_b,r.scalar_b,32);
    if(operation!=2)user_copy_from(slot,p.point,r.point,32);
    fs_release();unlock_kernel();int result=lume_curve_device_packet(&curve_device,&p);lock_kernel();
    if(result==0){user_copy_to(slot,r.output,p.output,32);if(r.cycles)user_copy_to(slot,r.cycles,&p.cycles,4);}
    lume_secret_clear(&p,sizeof p);curve_release(slot);return result;
}
