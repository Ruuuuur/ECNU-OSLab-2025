#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"

volatile static int started = 0;

int main()
{
    int cpuid = r_tp();
    if(cpuid == 0){
        print_init();
        pmem_init();
        kvm_init();
        kvm_inithart();
        trap_kernel_init();
        trap_kernel_inithart();
        printf("cpu %d is booting!\n", cpuid);

        __sync_synchronize();

        started = 1;
    }else{
        while(started == 0)
            ;

        __sync_synchronize();
        kvm_inithart();
        trap_kernel_inithart();
        printf("cpu %d is booting!\n", cpuid);
    }

    while(1)
        ;
}
