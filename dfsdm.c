#include <ch.h>
#include <hal.h>
#include "dfsdm.h"

/* Those defines are missing from the STM32F769 include, copied them from the L4 one. */
#define DFSDM_CHCFGR1_CKOUTDIV_Pos           (16U)
#define DFSDM_CHCFGR1_CKOUTDIV_Msk           (0xFFU << DFSDM_CHCFGR1_CKOUTDIV_Pos) /*!< 0x00FF0000 */
#define DFSDM_CHCFGR2_DTRBS_Pos              (3U)
#define DFSDM_FLTCR1_RCH_Pos                 (24U)
#define DFSDM_FLTCR1_RDMAEN_Pos              (21U)
#define DFSDM_FLTFCR_IOSR_Pos                (0U)
#define DFSDM_FLTFCR_FOSR_Pos                (16U)
#define DFSDM_FLTFCR_FORD_Pos                (29U)

/* Both DFSDM units are wired to the same DMA channel, but is changed depending
 * on the DMA stream. Stream 0/4 go to FLT0, 1/5 FLT1, etc.
 *
 * See Table 28 (DMA2 request mapping) in the STM32F7 reference manual for
 * complete list.
 * */
#define DFSDM_FLT0_DMA_CHN 8
#define DFSDM_FLT1_DMA_CHN 8

/* We initialize it so that it is forced into the .data section */
__attribute__((section(".nocache")))
static volatile int32_t samples[DFSDM_SAMPLE_LEN] = {42};

BSEMAPHORE_DECL(samples_full, true);

static size_t samples_index = 0;

static const stm32_dma_stream_t *left_dma_stream, *right_dma_stream;

static void dfsdm_serve_dma_interrupt(void *p, uint32_t flags)
{
    chSysLockFromISR();
    chBSemSignalI(&samples_full);
    chSysUnlockFromISR();
    palToggleLine(LINE_LED1_RED);
}

void dfsdm_init(void)
{
    /* Send clock to peripheral. */
    rccEnableAPB2(RCC_APB2ENR_DFSDM1EN, true);

    /* Configure DFSDM clock output (must be before enabling interface).
     *
     * The clock output is used by the microphones to send their data out.
     * DFSDM is on APB2 @ 108 Mhz. The MP34DT01 MEMS microphone runs @ 2.4 Mhz,
     * requiring a prescaler of 46.
     */
    const unsigned clkout_div = 45;
    DFSDM1_Channel0->CHCFGR1 |= (clkout_div & 0xff) << DFSDM_CHCFGR1_CKOUTDIV_Pos;

    /* Enable DFSDM interface */
    DFSDM1_Channel0->CHCFGR1 |= DFSDM_CHCFGR1_DFSDMEN;

    /* Serial input configuration.
     *
     * The two microphones (left and right) are connected to the same input pin.
     * As the microphone dont have a clock output, we reuse the internal clock.
     *
     * Channel 0 is connected on the input from channel 1 (CHINSEL=1)
     * Channel 0 data are on rising edge (SITP=0), while channel 1 are on falling edge(SITP=1).
     */
    DFSDM1_Channel0->CHCFGR1 |= DFSDM_CHCFGR1_CHINSEL;
    DFSDM1_Channel0->CHCFGR1 |= DFSDM_CHCFGR1_SPICKSEL_0;

    DFSDM1_Channel1->CHCFGR1 |= DFSDM_CHCFGR1_SPICKSEL_0;
    DFSDM1_Channel1->CHCFGR1 |= DFSDM_CHCFGR1_SITP_0;

    /* Enable channel 0 and 1. */
    DFSDM1_Channel0->CHCFGR1 |= DFSDM_CHCFGR1_CHEN;
    DFSDM1_Channel1->CHCFGR1 |= DFSDM_CHCFGR1_CHEN;

    /* Filter units configuration:
     * - Fast mode enabled
     * - Corresponding channel must be selected
     * - Continuous mode
     * - For channel 1: start synchronously with channel 0
     * - Sinc3 filter (from ST application example)
     * - Oversampling factor (OF) = 55, integrator oversampling (IF) = 1
     *   -> acquisition rate = APB2 / (clkout_div * OF * IF)
     *                       = 108 Mhz / (45 * 55) = 43.6 Khz.
     *   -> resolution = +/- (OF)^3
     *                 = +/- 166375
     *                 = ~ 19 bits (including sign bit)
     *    For details on filter configuration see section 17.3.8 of the
     *    reference manual (Digital Filter Configuration).
     *
     * TODO: Add DMA/IRQ support
     * TODO: Get to a precise 44.1 Khz clock using audio PLL
     */
    DFSDM1_Filter0->FLTCR1 = DFSDM_FLTCR1_FAST \
                             | (1 << DFSDM_FLTCR1_RDMAEN_Pos)
                             | (0 << DFSDM_FLTCR1_RCH_Pos);     /* channel */
    DFSDM1_Filter0->FLTFCR = (3 << DFSDM_FLTFCR_FORD_Pos)       /* filter order */ \
                             | (55 << DFSDM_FLTFCR_FOSR_Pos)    /* filter oversampling */ \
                             | (0 << DFSDM_FLTFCR_IOSR_Pos);   /* integrator oversampling */

    /* Filter 1 is identical, except that RSYNC is enabled. */
    DFSDM1_Filter1->FLTCR1 = DFSDM_FLTCR1_FAST \
                             | DFSDM_FLTCR1_RSYNC \
                             | (1 << DFSDM_FLTCR1_RDMAEN_Pos)
                             | (0 << DFSDM_FLTCR1_RCH_Pos);     /* channel */
    DFSDM1_Filter1->FLTFCR = (3 << DFSDM_FLTFCR_FORD_Pos)       /* filter order */ \
                             | (55 << DFSDM_FLTFCR_FOSR_Pos)    /* filter oversampling */ \
                             | (0 << DFSDM_FLTFCR_IOSR_Pos);   /* integrator oversampling */


    /* Enable the filters */
    DFSDM1_Filter0->FLTCR1 |= DFSDM_FLTCR1_DFEN;
    //DFSDM1_Filter1->FLTCR1 |= DFSDM_FLTCR1_DFEN;

    /* Allocate DMA streams. */
    bool b;
    left_dma_stream = STM32_DMA_STREAM(STM32_DFSDM_MICROPHONE_LEFT_DMA_STREAM);
    b = dmaStreamAllocate(left_dma_stream,
            STM32_DFSDM_MICROPHONE_LEFT_DMA_STREAM,
            dfsdm_serve_dma_interrupt,
            NULL);
    osalDbgAssert(!b, "stream already allocated");

    right_dma_stream = STM32_DMA_STREAM(STM32_DFSDM_MICROPHONE_RIGHT_DMA_STREAM);
    b = dmaStreamAllocate(right_dma_stream,
            STM32_DFSDM_MICROPHONE_RIGHT_DMA_STREAM,
            dfsdm_serve_dma_interrupt,
            NULL);
    osalDbgAssert(!b, "stream already allocated");

    dmaStreamSetPeripheral(left_dma_stream, &DFSDM1_Filter0->FLTRDATAR);
    dmaStreamSetPeripheral(right_dma_stream, &DFSDM1_Filter1->FLTRDATAR);
}

void dfsdm_start(void)
{
    /* Enable continuous conversion. */
    DFSDM1_Filter0->FLTCR1 |= DFSDM_FLTCR1_RCONT;
    DFSDM1_Filter1->FLTCR1 |= DFSDM_FLTCR1_RCONT;

    /* Configure DMA mode */
    uint32_t dma_mode = STM32_DMA_CR_CHSEL(DFSDM_FLT0_DMA_CHN) |
                  STM32_DMA_CR_PL(STM32_DFSDM_MICROPHONE_LEFT_DMA_PRIORITY) |
                  STM32_DMA_CR_DIR_P2M |
                  STM32_DMA_CR_MSIZE_WORD | STM32_DMA_CR_PSIZE_WORD |
                  STM32_DMA_CR_MINC        | STM32_DMA_CR_TCIE        |
                  STM32_DMA_CR_DMEIE       | STM32_DMA_CR_TEIE;

    dmaStreamSetMemory0(left_dma_stream, samples);
    dmaStreamSetTransactionSize(left_dma_stream, DFSDM_SAMPLE_LEN);
    dmaStreamSetMode(left_dma_stream, dma_mode);
    dmaStreamEnable(left_dma_stream);

    /* Start acquisition */
    DFSDM1_Filter0->FLTCR1 |= DFSDM_FLTCR1_RSWSTART;
}

void dfsdm_stop(void)
{
    DFSDM1_Filter0->FLTCR1 &= ~DFSDM_FLTCR1_RCONT;
    DFSDM1_Filter1->FLTCR1 &= ~DFSDM_FLTCR1_RCONT;

    nvicDisableVector(DFSDM1_FLT0_IRQn);
}

int32_t *dfsdm_wait(void)
{
    /* Wait for all samples to have been acquired. */
    chBSemWait(&samples_full);
    return samples;
}
