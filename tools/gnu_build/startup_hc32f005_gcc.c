#include <stdint.h>

typedef void (*isr_handler_t)(void);

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

extern void SystemInit(void);
extern int32_t main(void);
extern const isr_handler_t vector_table[];

void Default_Handler(void)
{
    for (;;) {
    }
}

#define WEAK_DEFAULT_HANDLER(name) \
    void name(void) __attribute__((weak, alias("Default_Handler")))

WEAK_DEFAULT_HANDLER(NMI_Handler);
WEAK_DEFAULT_HANDLER(HardFault_Handler);
WEAK_DEFAULT_HANDLER(SVC_Handler);
WEAK_DEFAULT_HANDLER(PendSV_Handler);
WEAK_DEFAULT_HANDLER(SysTick_Handler);
WEAK_DEFAULT_HANDLER(PORT0_IRQHandler);
WEAK_DEFAULT_HANDLER(PORT1_IRQHandler);
WEAK_DEFAULT_HANDLER(PORT2_IRQHandler);
WEAK_DEFAULT_HANDLER(PORT3_IRQHandler);
WEAK_DEFAULT_HANDLER(DMAC_IRQHandler);
WEAK_DEFAULT_HANDLER(TIM3_IRQHandler);
WEAK_DEFAULT_HANDLER(UART0_IRQHandler);
WEAK_DEFAULT_HANDLER(UART1_IRQHandler);
WEAK_DEFAULT_HANDLER(LPUART_IRQHandler);
WEAK_DEFAULT_HANDLER(SPI_IRQHandler);
WEAK_DEFAULT_HANDLER(I2C_IRQHandler);
WEAK_DEFAULT_HANDLER(TIM0_IRQHandler);
WEAK_DEFAULT_HANDLER(TIM1_IRQHandler);
WEAK_DEFAULT_HANDLER(TIM2_IRQHandler);
WEAK_DEFAULT_HANDLER(LPTIM_IRQHandler);
WEAK_DEFAULT_HANDLER(TIM4_IRQHandler);
WEAK_DEFAULT_HANDLER(TIM5_IRQHandler);
WEAK_DEFAULT_HANDLER(TIM6_IRQHandler);
WEAK_DEFAULT_HANDLER(PCA_IRQHandler);
WEAK_DEFAULT_HANDLER(WDT_IRQHandler);
WEAK_DEFAULT_HANDLER(RTC_IRQHandler);
WEAK_DEFAULT_HANDLER(ADC_IRQHandler);
WEAK_DEFAULT_HANDLER(VC0_IRQHandler);
WEAK_DEFAULT_HANDLER(VC1_IRQHandler);
WEAK_DEFAULT_HANDLER(LVD_IRQHandler);
WEAK_DEFAULT_HANDLER(FLASH_RAM_IRQHandler);
WEAK_DEFAULT_HANDLER(CLKTRIM_IRQHandler);

void Reset_Handler(void)
{
    uint32_t *source;
    uint32_t *destination;

    /* Match the vendor Flash startup instead of inheriting a vector base
     * from a ROM/debugger entry path. */
    *(volatile uint32_t *)0xE000ED08u = (uint32_t)vector_table;
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");

    source = &_sidata;
    destination = &_sdata;
    while (destination < &_edata) {
        *destination++ = *source++;
    }

    destination = &_sbss;
    while (destination < &_ebss) {
        *destination++ = 0u;
    }

    SystemInit();
    (void)main();
    for (;;) {
    }
}

__attribute__((section(".isr_vector"), used, aligned(256)))
const isr_handler_t vector_table[] = {
    (isr_handler_t)&_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    SVC_Handler,
    0,
    0,
    PendSV_Handler,
    SysTick_Handler,
    PORT0_IRQHandler,
    PORT1_IRQHandler,
    PORT2_IRQHandler,
    PORT3_IRQHandler,
    DMAC_IRQHandler,
    TIM3_IRQHandler,
    UART0_IRQHandler,
    UART1_IRQHandler,
    LPUART_IRQHandler,
    0,
    SPI_IRQHandler,
    0,
    I2C_IRQHandler,
    0,
    TIM0_IRQHandler,
    TIM1_IRQHandler,
    TIM2_IRQHandler,
    LPTIM_IRQHandler,
    TIM4_IRQHandler,
    TIM5_IRQHandler,
    TIM6_IRQHandler,
    PCA_IRQHandler,
    WDT_IRQHandler,
    RTC_IRQHandler,
    ADC_IRQHandler,
    0,
    VC0_IRQHandler,
    VC1_IRQHandler,
    LVD_IRQHandler,
    0,
    FLASH_RAM_IRQHandler,
    CLKTRIM_IRQHandler
};
