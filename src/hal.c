#include "hal.h"
#include "adc.h"
#include "ddl.h"
#include "gpio.h"
#include "sysctrl.h"
static volatile uint32_t ms;
static uint32_t reload;
static bool adc_ready;
void SysTick_Handler(void) { ++ms; }
uint32_t hal_millis(void) { return ms; }
void hal_delay_us(uint32_t us) {
    uint32_t n = (SystemCoreClock + 999999u) / 1000000u;
    if (!n)
        n = 1u;
    while (us--) {
        uint32_t k;
        for (k = 0u; k < n; ++k)
            __NOP();
    }
}
bool hal_clock_init(void) {
    uint32_t hz = Sysctrl_GetHClkFreq();
    if (hz < 1000u || hz > 32000000u)
        return false;
    SystemCoreClock = hz;
    reload = hz / 1000u;
    SysTick->CTRL = 0u;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
    return SysTick_Config(reload) == 0u;
}
bool hal_clock_ok(void) {
    return reload && SysTick->LOAD == reload - 1u && (SysTick->CTRL & 7u) == 7u &&
           __get_PRIMASK() == 0u;
}
static void write_pin(en_gpio_port_t port, en_gpio_pin_t pin, bool high) {
    uint32_t saved = __get_PRIMASK();
    __disable_irq();
    Gpio_WriteOutputIO(port, pin, high ? TRUE : FALSE);
    __set_PRIMASK(saved);
}
static bool pin_cfg(en_gpio_port_t port, en_gpio_pin_t pin, bool input, bool level, bool pull,
                    bool od) {
    stc_gpio_cfg_t c;
    DDL_ZERO_STRUCT(c);
    c.enDir = input ? GpioDirIn : GpioDirOut;
    c.enDrv = GpioDrvH;
    c.enPu = pull ? GpioPuEnable : GpioPuDisable;
    c.enPd = GpioPdDisable;
    c.enOD = od ? GpioOdEnable : GpioOdDisable;
    c.bOutputVal = level ? TRUE : FALSE;
    return Gpio_Init(port, pin, &c) == Ok;
}
void hal_heater(bool on) { write_pin(GpioPort0, GpioPin3, on); }
void hal_fan(bool on) { write_pin(GpioPort3, GpioPin1, on); }
void hal_beep(bool on) { write_pin(GpioPort3, GpioPin4, on); }
void hal_backlight(bool on) { write_pin(GpioPort0, GpioPin2, on); }
void hal_led(unsigned key, bool on) {
    const en_gpio_pin_t pins[3] = {GpioPin6, GpioPin5, GpioPin3};
    if (key < 3u)
        write_pin(GpioPort3, pins[key], !on);
}
bool hal_key(unsigned key) {
    if (key == 0u)
        return Gpio_GetInputIO(GpioPort3, GpioPin2) == FALSE;
    if (key == 1u)
        return Gpio_GetInputIO(GpioPort2, GpioPin3) == FALSE;
    return key == 2u && Gpio_GetInputIO(GpioPort2, GpioPin6) == FALSE;
}
static void adc_config(stc_adc_cfg_t *a) {
    DDL_ZERO_STRUCT(*a);
    a->enAdcOpMode = AdcNormalMode;
    a->enAdcClkSel = AdcClkSysTDiv2;
    a->enAdcSampTimeSel = AdcSampTime12Clk;
    a->enAdcRefVolSel = RefVolSelAVDD;
    a->bAdcInBufEn = FALSE;
    a->enAdcTrig0Sel = AdcTrigDisable;
    a->enAdcTrig1Sel = AdcTrigDisable;
}
bool hal_early_init(void) {
    stc_adc_cfg_t a;
    bool ok = true;
    if (Sysctrl_SetPeripheralGate(SysctrlPeripheralGpio, TRUE) != Ok)
        return false;
    ok = pin_cfg(GpioPort0, GpioPin3, false, false, false, false) && ok;
    ok = pin_cfg(GpioPort3, GpioPin4, false, false, false, false) && ok;
    ok = pin_cfg(GpioPort0, GpioPin2, false, false, false, false) && ok;
    ok = pin_cfg(GpioPort3, GpioPin6, false, true, false, false) && ok;
    ok = pin_cfg(GpioPort3, GpioPin5, false, true, false, false) && ok;
    ok = pin_cfg(GpioPort3, GpioPin3, false, true, false, false) && ok;
    ok = pin_cfg(GpioPort3, GpioPin2, true, true, true, false) && ok;
    ok = pin_cfg(GpioPort2, GpioPin3, true, true, true, false) && ok;
    ok = pin_cfg(GpioPort2, GpioPin6, true, true, true, false) && ok;
    ok = pin_cfg(GpioPort0, GpioPin1, false, true, false, false) && ok;
    ok = pin_cfg(GpioPort2, GpioPin5, false, true, false, false) && ok;
    ok = pin_cfg(GpioPort1, GpioPin4, false, true, true, true) && ok;
    ok = pin_cfg(GpioPort1, GpioPin5, false, true, true, true) && ok;
    adc_ready = false;
    if (Sysctrl_SetPeripheralGate(SysctrlPeripheralAdcBgr, TRUE) != Ok)
        return false;
    if (Gpio_SetAnalogMode(GpioPort2, GpioPin4) != Ok)
        return false;
    M0P_BGR->CR_f.BGR_EN = 1u;
    Adc_Enable();
    hal_delay_us(20u);
    adc_config(&a);
    adc_ready = Adc_Init(&a) == Ok;
    return adc_ready && ok;
}
bool hal_lcd_pins_init(void) {
    if (Sysctrl_SetFunc(SysctrlSWDUseIOEn, TRUE) != Ok)
        return false;
    return pin_cfg(GpioPort2, GpioPin7, false, true, false, false) &&
           pin_cfg(GpioPort3, GpioPin1, false, false, false, false);
}
bool hal_adc(uint16_t *mean_x10) {
    stc_adc_cfg_t a;
    stc_adc_norm_cfg_t n;
    uint32_t sum = 0u;
    unsigned i;
    if (!adc_ready || !mean_x10)
        return false;
    adc_config(&a);
    DDL_ZERO_STRUCT(n);
    n.enAdcNormModeCh = AdcExInputCH0;
    n.bAdcResultAccEn = FALSE;
    if (Adc_ConfigNormMode(&a, &n) != Ok)
        return false;
    for (i = 0u; i < 9u; ++i) {
        uint32_t timeout = 1024u;
        uint16_t value;
        Adc_Start();
        while (timeout && Adc_PollBusyState() != FALSE)
            --timeout;
        if (!timeout) {
            Adc_Stop();
            return false;
        }
        if (Adc_GetResult(&value) != Ok)
            return false;
        sum += value;
    }
    *mean_x10 = (uint16_t)(sum * 10u / 9u);
    return true;
}
void hal_scl(bool high) { write_pin(GpioPort1, GpioPin4, high); }
bool hal_scl_read(void) { return Gpio_GetInputIO(GpioPort1, GpioPin4) != FALSE; }
void hal_sda_output(bool high) { (void)pin_cfg(GpioPort1, GpioPin5, false, high, true, true); }
void hal_sda_input(void) { (void)pin_cfg(GpioPort1, GpioPin5, true, true, false, false); }
bool hal_sda_read(void) { return Gpio_GetInputIO(GpioPort1, GpioPin5) != FALSE; }
void hal_lcd_cs(bool high) { write_pin(GpioPort0, GpioPin1, high); }
void hal_lcd_wr(bool high) { write_pin(GpioPort2, GpioPin5, high); }
void hal_lcd_data(bool high) { write_pin(GpioPort2, GpioPin7, high); }
