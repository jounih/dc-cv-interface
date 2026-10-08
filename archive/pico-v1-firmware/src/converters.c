#include "converters.h"
#include "board.h"
#include "conv_codec.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "cv.pio.h"

#define DAC_PIO pio0
#define DAC_SM  0
#define ADC_PIO pio0
#define ADC_SM  1

uint32_t dac_ring[CONV_RING * CV_N_OUT] __attribute__((aligned(16)));
uint32_t adc_ring[CONV_RING * 6] __attribute__((aligned(16)));

static int dac_dma, dac_ctl, adc_dma, adc_ctl;
static uint32_t *dac_ring_start = dac_ring;
static uint32_t *adc_ring_start = adc_ring;

// ------------------------------------------------------------------ DAC
static void dac_put_blocking(uint32_t frame) {
    pio_sm_put_blocking(DAC_PIO, DAC_SM, frame);
}

static void dac_drain(void) {
    while (!pio_sm_is_tx_fifo_empty(DAC_PIO, DAC_SM)) tight_loop_contents();
    busy_wait_us_32(2);   // last frame (~1.4 us) leaves the shift register
}

void conv_dac_init(const uint16_t *zero_codes) {
    uint offset = pio_add_program(DAC_PIO, &spi_frame_tx_program);
    spi_frame_tx_init(DAC_PIO, DAC_SM, offset, PIN_DAC_SCLK, PIN_DAC_DIN, 32);

    // 1) Software reset = power-on reset: B grade -> midscale, internal ref off.
    //    With the reference off every DAC output and the offset node Vb are 0 V,
    //    so the jacks read 0 V regardless of codes (also after a watchdog reboot).
    dac_put_blocking(dac8568_soft_reset());
    dac_drain();
    busy_wait_us_32(50);
    // 2) Write the calibrated 0 V codes and update all channels.
    uint32_t frames[CV_N_OUT];
    dac8568_encode_sample(zero_codes, CV_N_OUT, frames);
    for (int i = 0; i < CV_N_OUT; i++) dac_put_blocking(frames[i]);
    // Unused DAC channels (4-out build) go to midscale-ish 0 V code too.
    for (int i = CV_N_OUT; i < 8; i++)
        dac_put_blocking(dac8568_frame(DAC8568_C_WRITE_UPDATE, (uint32_t)i, zero_codes[0], 0));
    // 3) Reference on (static mode). Vdac and Vb rise together: output stays ~0 V.
    dac_put_blocking(dac8568_ref_on());
    dac_drain();
}

void conv_dac_start(void) {
    uint16_t x, y;
    cv_dma_timer_frac(clock_get_hz(clk_sys), CV_SAMPLE_RATE * CV_N_OUT, &x, &y);
    int timer = dma_claim_unused_timer(true);
    dma_timer_set_fraction((uint)timer, x, y);

    dac_dma = dma_claim_unused_channel(true);
    dac_ctl = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config((uint)dac_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, dma_get_timer_dreq((uint)timer));
    channel_config_set_chain_to(&c, (uint)dac_ctl);
    dma_channel_configure((uint)dac_dma, &c, &DAC_PIO->txf[DAC_SM], dac_ring, CONV_RING * CV_N_OUT, false);

    // Control channel re-arms the data channel at the ring start (works on RP2040 too).
    dma_channel_config k = dma_channel_get_default_config((uint)dac_ctl);
    channel_config_set_transfer_data_size(&k, DMA_SIZE_32);
    channel_config_set_read_increment(&k, false);
    channel_config_set_write_increment(&k, false);
    dma_channel_configure((uint)dac_ctl, &k, &dma_hw->ch[dac_dma].al3_read_addr_trig, &dac_ring_start, 1, false);

    dma_channel_start((uint)dac_dma);
}

uint32_t conv_dac_hw_sample(void) {
    uint32_t addr = dma_hw->ch[dac_dma].read_addr;
    uint32_t word = (addr - (uint32_t)(uintptr_t)dac_ring) / 4u;
    return (word / CV_N_OUT) % CONV_RING;
}

// ------------------------------------------------------------------ ADC
static inline void bb_delay(void) { busy_wait_at_least_cycles(20); }

// Bit-banged SPI mode 1 frame (used only during configuration).
static void ads_xfer(const uint32_t *tx, uint32_t *rx, int words) {
    gpio_put(PIN_ADC_CS, 0);
    bb_delay();
    for (int w = 0; w < words; w++) {
        uint32_t in = 0;
        for (int b = 23; b >= 0; b--) {
            gpio_put(PIN_ADC_DIN, (tx[w] >> b) & 1u);
            gpio_put(PIN_ADC_SCLK, 1);
            bb_delay();
            in = (in << 1) | (uint32_t)gpio_get(PIN_ADC_DOUT);
            gpio_put(PIN_ADC_SCLK, 0);
            bb_delay();
        }
        rx[w] = in;
    }
    bb_delay();
    gpio_put(PIN_ADC_CS, 1);
    bb_delay();
}

static uint16_t ads_cmd(uint16_t cmd, uint16_t data, bool with_data) {
    uint32_t tx[ADS_FRAME_WORDS] = {0}, rx[ADS_FRAME_WORDS];
    tx[0] = ads_word(cmd);
    if (with_data) tx[1] = ads_word(data);
    ads_xfer(tx, rx, ADS_FRAME_WORDS);
    // The response to a command arrives in the next frame.
    uint32_t nul[ADS_FRAME_WORDS] = {0};
    ads_xfer(nul, rx, ADS_FRAME_WORDS);
    return ads_resp16(rx[0]);
}

bool conv_adc_init(uint16_t *id_out) {
    // 6.144 MHz CLKIN from GPOUT0: 150 MHz / 24.4140625 (exact in 16.16).
    uint32_t di; uint16_t df;
    cv_gpout_div(clock_get_hz(clk_sys), ADS_CLKIN_HZ, &di, &df);
    clock_gpio_init_int_frac16(PIN_ADC_CLKIN, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS, di, df);

    const uint outs[] = {PIN_ADC_SCLK, PIN_ADC_CS, PIN_ADC_DIN, PIN_ADC_RESET};
    for (unsigned i = 0; i < 4; i++) { gpio_init(outs[i]); gpio_set_dir(outs[i], GPIO_OUT); }
    gpio_put(PIN_ADC_CS, 1);
    gpio_put(PIN_ADC_SCLK, 0);
    gpio_put(PIN_ADC_DIN, 0);
    gpio_init(PIN_ADC_DOUT); gpio_set_dir(PIN_ADC_DOUT, GPIO_IN); gpio_pull_down(PIN_ADC_DOUT);
    gpio_init(PIN_ADC_DRDY); gpio_set_dir(PIN_ADC_DRDY, GPIO_IN); gpio_pull_up(PIN_ADC_DRDY);

    // Hardware reset: SYNC/RESET low > 2048 CLKIN periods (333 us).
    gpio_put(PIN_ADC_RESET, 0);
    sleep_ms(1);
    gpio_put(PIN_ADC_RESET, 1);
    sleep_ms(1);

    uint16_t id = ads_cmd(ads_rreg(ADS_REG_ID, 1), 0, false);
    if (id_out) *id_out = id;
    if (!ads_id_ok(id)) return false;
    bool ok = ads_cmd(ads_wreg(ADS_REG_MODE, 1), ADS_MODE_VALUE, true) == ads_wreg_ack(ADS_REG_MODE, 1);
    ok &= ads_cmd(ads_wreg(ADS_REG_CLOCK, 1), ADS_CLOCK_VALUE, true) == ads_wreg_ack(ADS_REG_CLOCK, 1);
    ok &= ads_cmd(ads_rreg(ADS_REG_CLOCK, 1), 0, false) == ADS_CLOCK_VALUE;
    return ok;
}

void conv_adc_start(void) {
    gpio_put(PIN_ADC_DIN, 0);   // NULL command for every streamed frame
    uint offset = pio_add_program(ADC_PIO, &ads131_rx_program);
    ads131_rx_init(ADC_PIO, ADC_SM, offset, PIN_ADC_SCLK, PIN_ADC_CS, PIN_ADC_DOUT);

    adc_dma = dma_claim_unused_channel(true);
    adc_ctl = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config((uint)adc_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(ADC_PIO, ADC_SM, false));
    channel_config_set_chain_to(&c, (uint)adc_ctl);
    dma_channel_configure((uint)adc_dma, &c, adc_ring, &ADC_PIO->rxf[ADC_SM], CONV_RING * 6, false);

    dma_channel_config k = dma_channel_get_default_config((uint)adc_ctl);
    channel_config_set_transfer_data_size(&k, DMA_SIZE_32);
    channel_config_set_read_increment(&k, false);
    channel_config_set_write_increment(&k, false);
    dma_channel_configure((uint)adc_ctl, &k, &dma_hw->ch[adc_dma].al2_write_addr_trig, &adc_ring_start, 1, false);

    dma_channel_start((uint)adc_dma);
}

uint32_t conv_adc_hw_sample(void) {
    uint32_t addr = dma_hw->ch[adc_dma].write_addr;
    uint32_t word = (addr - (uint32_t)(uintptr_t)adc_ring) / 4u;
    return (word / 6u) % CONV_RING;
}
